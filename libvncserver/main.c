/*
 *  This file is called main.c, because it contains most of the new functions
 *  for use with LibVNCServer.
 *
 *  LibVNCServer (C) 2001 Johannes E. Schindelin <Johannes.Schindelin@gmx.de>
 *  Original OSXvnc (C) 2001 Dan McGuirk <mcguirk@incompleteness.net>.
 *  Original Xvnc (C) 1999 AT&T Laboratories Cambridge.  
 *  All Rights Reserved.
 *
 *  see GPL (latest version) for full details
 */

#include <rfb/rfb.h>
#include <rfb/rfbregion.h>

#include <stdarg.h>
#include <errno.h>

#ifndef false
#define false 0
#define true -1
#endif

#ifdef LIBVNCSERVER_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include <signal.h>
#include <time.h>

#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
MUTEX(logMutex);
#endif

int rfbEnableLogging=1;

#ifdef LIBVNCSERVER_WORDS_BIGENDIAN
char rfbEndianTest = 0;
#else
char rfbEndianTest = -1;
#endif

/* from rfbserver.c */
void rfbIncrClientRef(rfbClientPtr cl);
void rfbDecrClientRef(rfbClientPtr cl);

void rfbLogEnable(int enabled) {
  rfbEnableLogging=enabled;
}

/*
 * rfbLog prints a time-stamped message to the log file (stderr).
 */

void
rfbDefaultLog(const char *format, ...)
{
    va_list args;
    char buf[256];
    time_t log_clock;

    if(!rfbEnableLogging)
      return;

    LOCK(logMutex);
    va_start(args, format);

    time(&log_clock);
    strftime(buf, 255, "%d/%m/%Y %X ", localtime(&log_clock));
    fprintf(stderr,buf);

    vfprintf(stderr, format, args);
    fflush(stderr);

    va_end(args);
    UNLOCK(logMutex);
}

rfbLogProc rfbLog=rfbDefaultLog;
rfbLogProc rfbErr=rfbDefaultLog;

void rfbLogPerror(const char *str)
{
    rfbErr("%s: %s\n", str, strerror(errno));
}

void rfbScheduleCopyRegion(rfbScreenInfoPtr rfbScreen,sraRegionPtr copyRegion,int dx,int dy)
{  
   rfbClientIteratorPtr iterator;
   rfbClientPtr cl;

   rfbUndrawCursor(rfbScreen);
  
   iterator=rfbGetClientIterator(rfbScreen);
   while((cl=rfbClientIteratorNext(iterator))) {
     LOCK(cl->updateMutex);
     if(cl->useCopyRect) {
       sraRegionPtr modifiedRegionBackup;
       if(!sraRgnEmpty(cl->copyRegion)) {
	  if(cl->copyDX!=dx || cl->copyDY!=dy) {
	     /* if a copyRegion was not yet executed, treat it as a
	      * modifiedRegion. The idea: in this case it could be
	      * source of the new copyRect or modified anyway. */
	     sraRgnOr(cl->modifiedRegion,cl->copyRegion);
	     sraRgnMakeEmpty(cl->copyRegion);
	  } else {
	     /* we have to set the intersection of the source of the copy
	      * and the old copy to modified. */
	     modifiedRegionBackup=sraRgnCreateRgn(copyRegion);
	     sraRgnOffset(modifiedRegionBackup,-dx,-dy);
	     sraRgnAnd(modifiedRegionBackup,cl->copyRegion);
	     sraRgnOr(cl->modifiedRegion,modifiedRegionBackup);
	     sraRgnDestroy(modifiedRegionBackup);
	  }
       }
	  
       sraRgnOr(cl->copyRegion,copyRegion);
       cl->copyDX = dx;
       cl->copyDY = dy;

       /* if there were modified regions, which are now copied,
	* mark them as modified, because the source of these can be overlapped
	* either by new modified or now copied regions. */
       modifiedRegionBackup=sraRgnCreateRgn(cl->modifiedRegion);
       sraRgnOffset(modifiedRegionBackup,dx,dy);
       sraRgnAnd(modifiedRegionBackup,cl->copyRegion);
       sraRgnOr(cl->modifiedRegion,modifiedRegionBackup);
       sraRgnDestroy(modifiedRegionBackup);

#if 0
       /* TODO: is this needed? Or does it mess up deferring? */
       /* while(!sraRgnEmpty(cl->copyRegion)) */ {
#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
	 if(!cl->screen->backgroundLoop)
#endif
	   {
	     sraRegionPtr updateRegion = sraRgnCreateRgn(cl->modifiedRegion);
	     sraRgnOr(updateRegion,cl->copyRegion);
	     UNLOCK(cl->updateMutex);
	     rfbSendFramebufferUpdate(cl,updateRegion);
	     sraRgnDestroy(updateRegion);
	     continue;
	   }
       }
#endif
     } else {
       sraRgnOr(cl->modifiedRegion,copyRegion);
     }
     TSIGNAL(cl->updateCond);
     UNLOCK(cl->updateMutex);
   }

   rfbReleaseClientIterator(iterator);
}

void rfbDoCopyRegion(rfbScreenInfoPtr rfbScreen,sraRegionPtr copyRegion,int dx,int dy)
{
   sraRectangleIterator* i;
   sraRect rect;
   int j,widthInBytes,bpp=rfbScreen->rfbServerFormat.bitsPerPixel/8,
    rowstride=rfbScreen->paddedWidthInBytes;
   char *in,*out;

   rfbUndrawCursor(rfbScreen);
  
   /* copy it, really */
   i = sraRgnGetReverseIterator(copyRegion,dx<0,dy<0);
   while(sraRgnIteratorNext(i,&rect)) {
     widthInBytes = (rect.x2-rect.x1)*bpp;
     out = rfbScreen->frameBuffer+rect.x1*bpp+rect.y1*rowstride;
     in = rfbScreen->frameBuffer+(rect.x1-dx)*bpp+(rect.y1-dy)*rowstride;
     if(dy<0)
       for(j=rect.y1;j<rect.y2;j++,out+=rowstride,in+=rowstride)
	 memmove(out,in,widthInBytes);
     else {
       out += rowstride*(rect.y2-rect.y1-1);
       in += rowstride*(rect.y2-rect.y1-1);
       for(j=rect.y2-1;j>=rect.y1;j--,out-=rowstride,in-=rowstride)
	 memmove(out,in,widthInBytes);
     }
   }
  
   rfbScheduleCopyRegion(rfbScreen,copyRegion,dx,dy);
}

void rfbDoCopyRect(rfbScreenInfoPtr rfbScreen,int x1,int y1,int x2,int y2,int dx,int dy)
{
  sraRegionPtr region = sraRgnCreateRect(x1,y1,x2,y2);
  rfbDoCopyRegion(rfbScreen,region,dx,dy);
}

void rfbScheduleCopyRect(rfbScreenInfoPtr rfbScreen,int x1,int y1,int x2,int y2,int dx,int dy)
{
  sraRegionPtr region = sraRgnCreateRect(x1,y1,x2,y2);
  rfbScheduleCopyRegion(rfbScreen,region,dx,dy);
}

void rfbMarkRegionAsModified(rfbScreenInfoPtr rfbScreen,sraRegionPtr modRegion)
{
   rfbClientIteratorPtr iterator;
   rfbClientPtr cl;

   iterator=rfbGetClientIterator(rfbScreen);
   while((cl=rfbClientIteratorNext(iterator))) {
     LOCK(cl->updateMutex);
     sraRgnOr(cl->modifiedRegion,modRegion);
     TSIGNAL(cl->updateCond);
     UNLOCK(cl->updateMutex);
   }

   rfbReleaseClientIterator(iterator);
}

void rfbMarkRectAsModified(rfbScreenInfoPtr rfbScreen,int x1,int y1,int x2,int y2)
{
   sraRegionPtr region;
   int i;

   if(x1>x2) { i=x1; x1=x2; x2=i; }
   if(x1<0) x1=0;
   if(x2>=rfbScreen->width) x2=rfbScreen->width-1;
   if(x1==x2) return;
   
   if(y1>y2) { i=y1; y1=y2; y2=i; }
   if(y1<0) y1=0;
   if(y2>=rfbScreen->height) y2=rfbScreen->height-1;
   if(y1==y2) return;
   
   region = sraRgnCreateRect(x1,y1,x2,y2);
   rfbMarkRegionAsModified(rfbScreen,region);
   sraRgnDestroy(region);
}

#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
static void *
clientOutput(void *data)
{
    rfbClientPtr cl = (rfbClientPtr)data;
    rfbBool haveUpdate;
    sraRegion* updateRegion;

    while (1) {
        haveUpdate = false;
        while (!haveUpdate) {
            if (cl->sock == -1) {
                /* Client has disconnected. */
                return NULL;
            }
	    LOCK(cl->updateMutex);
	    haveUpdate = FB_UPDATE_PENDING(cl);
	    if(!haveUpdate) {
		updateRegion = sraRgnCreateRgn(cl->modifiedRegion);
		haveUpdate = sraRgnAnd(updateRegion,cl->requestedRegion);
		sraRgnDestroy(updateRegion);
	    }
	    UNLOCK(cl->updateMutex);

            if (!haveUpdate) {
                WAIT(cl->updateCond, cl->updateMutex);
		UNLOCK(cl->updateMutex); /* we really needn't lock now. */
            }
        }
        
        /* OK, now, to save bandwidth, wait a little while for more
           updates to come along. */
        usleep(cl->screen->rfbDeferUpdateTime * 1000);

        /* Now, get the region we're going to update, and remove
           it from cl->modifiedRegion _before_ we send the update.
           That way, if anything that overlaps the region we're sending
           is updated, we'll be sure to do another update later. */
        LOCK(cl->updateMutex);
	updateRegion = sraRgnCreateRgn(cl->modifiedRegion);
        UNLOCK(cl->updateMutex);

        /* Now actually send the update. */
	rfbIncrClientRef(cl);
        rfbSendFramebufferUpdate(cl, updateRegion);
	rfbDecrClientRef(cl);

	sraRgnDestroy(updateRegion);
    }

    return NULL;
}

static void *
clientInput(void *data)
{
    rfbClientPtr cl = (rfbClientPtr)data;
    pthread_t output_thread;
    pthread_create(&output_thread, NULL, clientOutput, (void *)cl);

    while (1) {
        rfbProcessClientMessage(cl);
        if (cl->sock == -1) {
            /* Client has disconnected. */
            break;
        }
    }

    /* Get rid of the output thread. */
    LOCK(cl->updateMutex);
    TSIGNAL(cl->updateCond);
    UNLOCK(cl->updateMutex);
    IF_PTHREADS(pthread_join(output_thread, NULL));

    rfbClientConnectionGone(cl);

    return NULL;
}

static void*
listenerRun(void *data)
{
    rfbScreenInfoPtr rfbScreen=(rfbScreenInfoPtr)data;
    int client_fd;
    struct sockaddr_in peer;
    rfbClientPtr cl;
    size_t len;

    len = sizeof(peer);

    /* TODO: this thread wont die by restarting the server */
    while ((client_fd = accept(rfbScreen->rfbListenSock, 
                               (struct sockaddr*)&peer, &len)) >= 0) {
        cl = rfbNewClient(rfbScreen,client_fd);
        len = sizeof(peer);

	if (cl && !cl->onHold )
		rfbStartOnHoldClient(cl);
    }
    return(NULL);
}

void 
rfbStartOnHoldClient(rfbClientPtr cl)
{
    pthread_create(&cl->client_thread, NULL, clientInput, (void *)cl);
}

#else

void 
rfbStartOnHoldClient(rfbClientPtr cl)
{
	cl->onHold = FALSE;
}

#endif

void 
rfbRefuseOnHoldClient(rfbClientPtr cl)
{
    rfbCloseClient(cl);
    rfbClientConnectionGone(cl);
}

static void
defaultKbdAddEvent(rfbBool down, rfbKeySym keySym, rfbClientPtr cl)
{
}

void
defaultPtrAddEvent(int buttonMask, int x, int y, rfbClientPtr cl)
{
  rfbClientIteratorPtr iterator;
  rfbClientPtr other_client;

  if (x != cl->screen->cursorX || y != cl->screen->cursorY) {
    if (cl->screen->cursorIsDrawn)
      rfbUndrawCursor(cl->screen);
    LOCK(cl->screen->cursorMutex);
    if (!cl->screen->cursorIsDrawn) {
      cl->screen->cursorX = x;
      cl->screen->cursorY = y;
    }
    UNLOCK(cl->screen->cursorMutex);

    /* The cursor was moved by this client, so don't send CursorPos. */
    if (cl->enableCursorPosUpdates)
      cl->cursorWasMoved = FALSE;

    /* But inform all remaining clients about this cursor movement. */
    iterator = rfbGetClientIterator(cl->screen);
    while ((other_client = rfbClientIteratorNext(iterator)) != NULL) {
      if (other_client != cl && other_client->enableCursorPosUpdates) {
	other_client->cursorWasMoved = TRUE;
      }
    }
    rfbReleaseClientIterator(iterator);
  }
}

void defaultSetXCutText(char* text, int len, rfbClientPtr cl)
{
}

/* TODO: add a nice VNC or RFB cursor */

#if defined(WIN32) || defined(sparc) || !defined(NO_STRICT_ANSI)
static rfbCursor myCursor = 
{
   FALSE, FALSE, FALSE, FALSE,
   (unsigned char*)"\000\102\044\030\044\102\000",
   (unsigned char*)"\347\347\176\074\176\347\347",
   8, 7, 3, 3,
   0, 0, 0,
   0xffff, 0xffff, 0xffff,
   0
};
#else
static rfbCursor myCursor = 
{
   cleanup: FALSE,
   cleanupSource: FALSE,
   cleanupMask: FALSE,
   cleanupRichSource: FALSE,
   source: "\000\102\044\030\044\102\000",
   mask:   "\347\347\176\074\176\347\347",
   width: 8, height: 7, xhot: 3, yhot: 3,
   foreRed: 0, foreGreen: 0, foreBlue: 0,
   backRed: 0xffff, backGreen: 0xffff, backBlue: 0xffff,
   richSource: 0
};
#endif

rfbCursorPtr defaultGetCursorPtr(rfbClientPtr cl)
{
   return(cl->screen->cursor);
}

/* response is cl->authChallenge vncEncrypted with passwd */
rfbBool defaultPasswordCheck(rfbClientPtr cl,const char* response,int len)
{
  int i;
  char *passwd=vncDecryptPasswdFromFile(cl->screen->rfbAuthPasswdData);

  if(!passwd) {
    rfbErr("Couldn't read password file: %s\n",cl->screen->rfbAuthPasswdData);
    return(FALSE);
  }

  vncEncryptBytes(cl->authChallenge, passwd);

  /* Lose the password from memory */
  for (i = strlen(passwd); i >= 0; i--) {
    passwd[i] = '\0';
  }

  free(passwd);

  if (memcmp(cl->authChallenge, response, len) != 0) {
    rfbErr("rfbAuthProcessClientMessage: authentication failed from %s\n",
	   cl->host);
    return(FALSE);
  }

  return(TRUE);
}

/* for this method, rfbAuthPasswdData is really a pointer to an array
   of char*'s, where the last pointer is 0. */
rfbBool rfbCheckPasswordByList(rfbClientPtr cl,const char* response,int len)
{
  char **passwds;
  int i=0;

  for(passwds=(char**)cl->screen->rfbAuthPasswdData;*passwds;passwds++,i++) {
    uint8_t auth_tmp[CHALLENGESIZE];
    memcpy((char *)auth_tmp, (char *)cl->authChallenge, CHALLENGESIZE);
    vncEncryptBytes(auth_tmp, *passwds);

    if (memcmp(auth_tmp, response, len) == 0) {
      if(i>=cl->screen->rfbAuthPasswdFirstViewOnly)
	cl->viewOnly=TRUE;
      return(TRUE);
    }
  }

  rfbErr("rfbAuthProcessClientMessage: authentication failed from %s\n",
	 cl->host);
  return(FALSE);
}

void doNothingWithClient(rfbClientPtr cl)
{
}

enum rfbNewClientAction defaultNewClientHook(rfbClientPtr cl)
{
	return RFB_CLIENT_ACCEPT;
}

/*
 * Update server's pixel format in rfbScreenInfo structure. This
 * function is called from rfbGetScreen() and rfbNewFramebuffer().
 */

static void rfbInitServerFormat(rfbScreenInfoPtr rfbScreen, int bitsPerSample)
{
   rfbPixelFormat* format=&rfbScreen->rfbServerFormat;

   format->bitsPerPixel = rfbScreen->bitsPerPixel;
   format->depth = rfbScreen->depth;
   format->bigEndian = rfbEndianTest?FALSE:TRUE;
   format->trueColour = TRUE;
   rfbScreen->colourMap.count = 0;
   rfbScreen->colourMap.is16 = 0;
   rfbScreen->colourMap.data.bytes = NULL;

   if (format->bitsPerPixel == 8) {
     format->redMax = 7;
     format->greenMax = 7;
     format->blueMax = 3;
     format->redShift = 0;
     format->greenShift = 3;
     format->blueShift = 6;
   } else {
     format->redMax = (1 << bitsPerSample) - 1;
     format->greenMax = (1 << bitsPerSample) - 1;
     format->blueMax = (1 << bitsPerSample) - 1;
     if(rfbEndianTest) {
       format->redShift = 0;
       format->greenShift = bitsPerSample;
       format->blueShift = bitsPerSample * 2;
     } else {
       if(format->bitsPerPixel==8*3) {
	 format->redShift = bitsPerSample*2;
	 format->greenShift = bitsPerSample*1;
	 format->blueShift = 0;
       } else {
	 format->redShift = bitsPerSample*3;
	 format->greenShift = bitsPerSample*2;
	 format->blueShift = bitsPerSample;
       }
     }
   }
}

rfbScreenInfoPtr rfbGetScreen(int* argc,char** argv,
 int width,int height,int bitsPerSample,int samplesPerPixel,
 int bytesPerPixel)
{
   rfbScreenInfoPtr rfbScreen=malloc(sizeof(rfbScreenInfo));

   INIT_MUTEX(logMutex);

   if(width&3)
     rfbErr("WARNING: Width (%d) is not a multiple of 4. VncViewer has problems with that.\n",width);

   rfbScreen->autoPort=FALSE;
   rfbScreen->rfbClientHead=0;
   rfbScreen->rfbPort=5900;
   rfbScreen->socketInitDone=FALSE;

   rfbScreen->inetdInitDone = FALSE;
   rfbScreen->inetdSock=-1;

   rfbScreen->udpSock=-1;
   rfbScreen->udpSockConnected=FALSE;
   rfbScreen->udpPort=0;
   rfbScreen->udpClient=0;

   rfbScreen->maxFd=0;
   rfbScreen->rfbListenSock=-1;

   rfbScreen->httpInitDone=FALSE;
   rfbScreen->httpEnableProxyConnect=FALSE;
   rfbScreen->httpPort=0;
   rfbScreen->httpDir=NULL;
   rfbScreen->httpListenSock=-1;
   rfbScreen->httpSock=-1;

   rfbScreen->desktopName = "LibVNCServer";
   rfbScreen->rfbAlwaysShared = FALSE;
   rfbScreen->rfbNeverShared = FALSE;
   rfbScreen->rfbDontDisconnect = FALSE;
   rfbScreen->rfbAuthPasswdData = 0;
   rfbScreen->rfbAuthPasswdFirstViewOnly = 1;
   
   rfbScreen->width = width;
   rfbScreen->height = height;
   rfbScreen->bitsPerPixel = rfbScreen->depth = 8*bytesPerPixel;

   rfbScreen->passwordCheck = defaultPasswordCheck;

   rfbScreen->ignoreSIGPIPE = TRUE;

   /* disable progressive updating per default */
   rfbScreen->progressiveSliceHeight = 0;

   if(!rfbProcessArguments(rfbScreen,argc,argv)) {
     free(rfbScreen);
     return 0;
   }

#ifdef WIN32
   {
	   DWORD dummy=255;
	   GetComputerName(rfbScreen->rfbThisHost,&dummy);
   }
#else
   gethostname(rfbScreen->rfbThisHost, 255);
#endif

   rfbScreen->paddedWidthInBytes = width*bytesPerPixel;

   /* format */

   rfbInitServerFormat(rfbScreen, bitsPerSample);

   /* cursor */

   rfbScreen->cursorIsDrawn = FALSE;
   rfbScreen->dontSendFramebufferUpdate = FALSE;
   rfbScreen->cursorX=rfbScreen->cursorY=rfbScreen->underCursorBufferLen=0;
   rfbScreen->underCursorBuffer=NULL;
   rfbScreen->dontConvertRichCursorToXCursor = FALSE;
   rfbScreen->cursor = &myCursor;
   INIT_MUTEX(rfbScreen->cursorMutex);

   IF_PTHREADS(rfbScreen->backgroundLoop = FALSE);

   rfbScreen->rfbDeferUpdateTime=5;
   rfbScreen->maxRectsPerUpdate=50;

   /* proc's and hook's */

   rfbScreen->kbdAddEvent = defaultKbdAddEvent;
   rfbScreen->kbdReleaseAllKeys = doNothingWithClient;
   rfbScreen->ptrAddEvent = defaultPtrAddEvent;
   rfbScreen->setXCutText = defaultSetXCutText;
   rfbScreen->getCursorPtr = defaultGetCursorPtr;
   rfbScreen->setTranslateFunction = rfbSetTranslateFunction;
   rfbScreen->newClientHook = defaultNewClientHook;
   rfbScreen->displayHook = 0;

   /* initialize client list and iterator mutex */
   rfbClientListInit(rfbScreen);

   return(rfbScreen);
}

/*
 * Switch to another framebuffer (maybe of different size and color
 * format). Clients supporting NewFBSize pseudo-encoding will change
 * their local framebuffer dimensions if necessary.
 * NOTE: Rich cursor data should be converted to new pixel format by
 * the caller.
 */

void rfbNewFramebuffer(rfbScreenInfoPtr rfbScreen, char *framebuffer,
                       int width, int height,
                       int bitsPerSample, int samplesPerPixel,
                       int bytesPerPixel)
{
  rfbPixelFormat old_format;
  rfbBool format_changed = FALSE;
  rfbClientIteratorPtr iterator;
  rfbClientPtr cl;

  /* Remove the pointer */

  rfbUndrawCursor(rfbScreen);

  /* Update information in the rfbScreenInfo structure */

  old_format = rfbScreen->rfbServerFormat;

  if (width & 3)
    rfbErr("WARNING: New width (%d) is not a multiple of 4.\n", width);

  rfbScreen->width = width;
  rfbScreen->height = height;
  rfbScreen->bitsPerPixel = rfbScreen->depth = 8*bytesPerPixel;
  rfbScreen->paddedWidthInBytes = width*bytesPerPixel;

  rfbInitServerFormat(rfbScreen, bitsPerSample);

  if (memcmp(&rfbScreen->rfbServerFormat, &old_format,
             sizeof(rfbPixelFormat)) != 0) {
    format_changed = TRUE;
  }

  rfbScreen->frameBuffer = framebuffer;

  /* Adjust pointer position if necessary */

  if (rfbScreen->cursorX >= width)
    rfbScreen->cursorX = width - 1;
  if (rfbScreen->cursorY >= height)
    rfbScreen->cursorY = height - 1;

  /* For each client: */
  iterator = rfbGetClientIterator(rfbScreen);
  while ((cl = rfbClientIteratorNext(iterator)) != NULL) {

    /* Re-install color translation tables if necessary */

    if (format_changed)
      rfbScreen->setTranslateFunction(cl);

    /* Mark the screen contents as changed, and schedule sending
       NewFBSize message if supported by this client. */

    LOCK(cl->updateMutex);
    sraRgnDestroy(cl->modifiedRegion);
    cl->modifiedRegion = sraRgnCreateRect(0, 0, width, height);
    sraRgnMakeEmpty(cl->copyRegion);
    cl->copyDX = 0;
    cl->copyDY = 0;

    if (cl->useNewFBSize)
      cl->newFBSizePending = TRUE;

    TSIGNAL(cl->updateCond);
    UNLOCK(cl->updateMutex);
  }
  rfbReleaseClientIterator(iterator);
}

#ifdef LIBVNCSERVER_HAVE_LIBJPEG
extern void TightCleanup();
#endif

void rfbScreenCleanup(rfbScreenInfoPtr rfbScreen)
{
  rfbClientIteratorPtr i=rfbGetClientIterator(rfbScreen);
  rfbClientPtr cl,cl1=rfbClientIteratorNext(i);
  while(cl1) {
    cl=rfbClientIteratorNext(i);
    rfbClientConnectionGone(cl1);
    cl1=cl;
  }
  rfbReleaseClientIterator(i);
    
  /* TODO: hang up on all clients and free all reserved memory */
#define FREE_IF(x) if(rfbScreen->x) free(rfbScreen->x)
  FREE_IF(colourMap.data.bytes);
  FREE_IF(underCursorBuffer);
  TINI_MUTEX(rfbScreen->cursorMutex);
  if(rfbScreen->cursor)
    rfbFreeCursor(rfbScreen->cursor);
  free(rfbScreen);
#ifdef LIBVNCSERVER_HAVE_LIBJPEG
  TightCleanup();
#endif
}

void rfbInitServer(rfbScreenInfoPtr rfbScreen)
{
#ifdef WIN32
  WSADATA trash;
  int i=WSAStartup(MAKEWORD(2,2),&trash);
#endif
  rfbInitSockets(rfbScreen);
  httpInitSockets(rfbScreen);
  if(rfbScreen->ignoreSIGPIPE)
    signal(SIGPIPE,SIG_IGN);
}

#ifndef LIBVNCSERVER_HAVE_GETTIMEOFDAY
#include <fcntl.h>
#include <conio.h>
#include <sys/timeb.h>

void gettimeofday(struct timeval* tv,char* dummy)
{
   SYSTEMTIME t;
   GetSystemTime(&t);
   tv->tv_sec=t.wHour*3600+t.wMinute*60+t.wSecond;
   tv->tv_usec=t.wMilliseconds*1000;
}
#endif

/* defined in rfbserver.c, but kind of "private" */
rfbClientPtr rfbClientIteratorHead(rfbClientIteratorPtr i);

void
rfbProcessEvents(rfbScreenInfoPtr rfbScreen,long usec)
{
  rfbClientIteratorPtr i;
  rfbClientPtr cl,clPrev;
  struct timeval tv;

  if(usec<0)
    usec=rfbScreen->rfbDeferUpdateTime*1000;

  rfbCheckFds(rfbScreen,usec);
  httpCheckFds(rfbScreen);
#ifdef CORBA
  corbaCheckFds(rfbScreen);
#endif

  i = rfbGetClientIterator(rfbScreen);
  cl=rfbClientIteratorHead(i);
  while(cl) {
    if (cl->sock >= 0 && !cl->onHold && FB_UPDATE_PENDING(cl) &&
        !sraRgnEmpty(cl->requestedRegion)) {
      if(rfbScreen->rfbDeferUpdateTime == 0) {
	  rfbSendFramebufferUpdate(cl,cl->modifiedRegion);
      } else if(cl->startDeferring.tv_usec == 0) {
	gettimeofday(&cl->startDeferring,NULL);
	if(cl->startDeferring.tv_usec == 0)
	  cl->startDeferring.tv_usec++;
      } else {
	gettimeofday(&tv,NULL);
	if(tv.tv_sec < cl->startDeferring.tv_sec /* at midnight */
	   || ((tv.tv_sec-cl->startDeferring.tv_sec)*1000
	       +(tv.tv_usec-cl->startDeferring.tv_usec)/1000)
	     > rfbScreen->rfbDeferUpdateTime) {
	  cl->startDeferring.tv_usec = 0;
	  rfbSendFramebufferUpdate(cl,cl->modifiedRegion);
	}
      }
    }
    clPrev=cl;
    cl=rfbClientIteratorNext(i);
    if(clPrev->sock==-1)
      rfbClientConnectionGone(clPrev);
  }
  rfbReleaseClientIterator(i);
}

void rfbRunEventLoop(rfbScreenInfoPtr rfbScreen, long usec, rfbBool runInBackground)
{
  if(runInBackground) {
#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
       pthread_t listener_thread;

       rfbScreen->backgroundLoop = TRUE;

       pthread_create(&listener_thread, NULL, listenerRun, rfbScreen);
    return;
#else
    rfbErr("Can't run in background, because I don't have PThreads!\n");
    return;
#endif
  }

  if(usec<0)
    usec=rfbScreen->rfbDeferUpdateTime*1000;

  while(1)
    rfbProcessEvents(rfbScreen,usec);
}