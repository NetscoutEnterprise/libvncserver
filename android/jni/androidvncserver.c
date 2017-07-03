/*
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This project is an adaptation of the original fbvncserver for the iPAQ
 * and Zaurus.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <sys/stat.h>
#include <sys/sysmacros.h>             /* For makedev() */

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include <assert.h>
#include <errno.h>

/* libvncserver */
#include "rfb/rfb.h"
#include "rfb/keysym.h"

/*****************************************************************************/

/* Android does not use /dev/fb0. */
#define FB_DEVICE "/dev/graphics/fb0"
static char KBD_DEVICE[256] = "/dev/input/event3";
static char TOUCH_DEVICE[256] = "/dev/input/event1";
static struct fb_var_screeninfo scrinfo;
static int fbfd = -1;
static int uinputfd = -1;
static unsigned short int *fbmmap = MAP_FAILED;
static unsigned short int *vncbuf;
static unsigned short int *fbbuf;

/* Android already has 5900 bound natively. */
#define VNC_PORT 5901
static rfbScreenInfoPtr vncscr;

static int xmin, xmax;
static int ymin, ymax;

static int left_button_pressed = 0;

static int activeClients = 0;


/* No idea, just copied from fbvncserver as part of the frame differerencing
 * algorithm.  I will probably be later rewriting all of this. */
static struct varblock_t
{
	int min_i;
	int min_j;
	int max_i;
	int max_j;
	int r_offset;
	int g_offset;
	int b_offset;
	int rfb_xres;
	int rfb_maxy;
} varblock;

/*****************************************************************************/

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl);
static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl);

/*****************************************************************************/

static void init_fb(void)
{
	size_t pixels;
	size_t bytespp;

	if ((fbfd = open(FB_DEVICE, O_RDONLY)) == -1)
	{
		printf("cannot open fb device %s\n", FB_DEVICE);
		exit(EXIT_FAILURE);
	}

	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &scrinfo) != 0)
	{
		printf("ioctl error\n");
		exit(EXIT_FAILURE);
	}

	pixels = scrinfo.xres * scrinfo.yres;
	bytespp = scrinfo.bits_per_pixel / 8;

	fprintf(stderr, "xres=%d, yres=%d, xresv=%d, yresv=%d, xoffs=%d, yoffs=%d, bpp=%d\n", 
	  (int)scrinfo.xres, (int)scrinfo.yres,
	  (int)scrinfo.xres_virtual, (int)scrinfo.yres_virtual,
	  (int)scrinfo.xoffset, (int)scrinfo.yoffset,
	  (int)scrinfo.bits_per_pixel);

	fbmmap = mmap(NULL, pixels * bytespp, PROT_READ, MAP_SHARED, fbfd, 0);

	if (fbmmap == MAP_FAILED)
	{
		printf("mmap failed\n");
		exit(EXIT_FAILURE);
	}
}

static void cleanup_fb(void)
{
	if(fbfd != -1)
	{
		close(fbfd);
	}
}

static int init_uinput()
{
    // Setup from getting-started-with-uinput
	if((uinputfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) == -1)
	{
		printf("cannot open /dev/uinput\n");
		exit(EXIT_FAILURE);
	}

    if (ioctl(uinputfd, UI_SET_EVBIT, EV_KEY) == -1)
    {
        goto init_fail1;
    }

    if (ioctl(uinputfd, UI_SET_EVBIT, EV_REP) == -1)
    {
        goto init_fail2;
    }

    if (ioctl(uinputfd, UI_SET_EVBIT, EV_SYN) == -1)
        goto init_fail3;

    if (ioctl(uinputfd, UI_SET_EVBIT, EV_ABS) == -1)
    {
        goto init_fail3;
    }

    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_X) == -1)
    {
        goto init_fail4;
    }

    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_Y) == -1)
    {
        goto init_fail5;
    }

    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_MT_POSITION_X) == -1)
    {
        goto init_fail5;
    }
    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_MT_POSITION_X) == -1)
    {
        goto init_fail5;
    }
    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_MT_PRESSURE) == -1)
    {
        goto init_fail5;
    }
    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR) == -1)
    {
        goto init_fail5;
    }
    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) == -1)
    {
        goto init_fail5;
    }
    if (ioctl(uinputfd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) == -1)
    {
        goto init_fail5;
    }

    {
      int key;
      for (key = 0; key < KEY_MAX; key++)
      {
        if (ioctl(uinputfd, UI_SET_KEYBIT, key) == -1)
        {
          goto init_fail6;
        }
      }
    }

    {
        struct uinput_user_dev uidev;
        memset(&uidev, 0, sizeof(uidev));

        snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "androidvnc");
        uidev.id.bustype = BUS_VIRTUAL; //BUS_USB;
        uidev.id.vendor  = 1; //0x1234;
        uidev.id.product = 1; //0xfedc;
        uidev.id.version = 1;

        uidev.absmin[ABS_X] = 0;
        uidev.absmax[ABS_X] = 0x7fff;
        uidev.absfuzz[ABS_X] = 0;
        uidev.absflat[ABS_X] = 0;

        uidev.absmin[ABS_Y] = 0;
        uidev.absmax[ABS_Y] = 0x7fff;
        uidev.absfuzz[ABS_Y] = 0;
        uidev.absflat[ABS_Y] = 0;

        if (write(uinputfd, &uidev, sizeof(uidev)) != sizeof(uidev))
        {
            goto init_fail7;
        }
        
        if(ioctl(uinputfd, UI_DEV_CREATE) == -1)
        {
            goto init_fail8;
        }
    }

    return 0;

    init_fail8:
    init_fail7:
    init_fail6:
    init_fail5:
    init_fail4:
    init_fail3:
    init_fail2:
    init_fail1:

    return -1;
        
}

static void cleanup_uinput()
{
	if(uinputfd != -1)
	{
		close(uinputfd);
	}
}

void clientGoneHook(struct _rfbClientRec* cl) {
    activeClients--;
    return;
}

enum rfbNewClientAction clientHook(struct _rfbClientRec* cl) {
    cl->clientGoneHook = clientGoneHook;
    activeClients++;
  return RFB_CLIENT_ACCEPT;
}

/*****************************************************************************/

static void init_fb_server(int argc, char **argv)
{
	printf("Initializing server...\n");

	/* Allocate the VNC server buffer to be managed (not manipulated) by 
	 * libvncserver. */
	vncbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(vncbuf != NULL);

	/* Allocate the comparison buffer for detecting drawing updates from frame
	 * to frame. */
	fbbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(fbbuf != NULL);

	/* TODO: This assumes scrinfo.bits_per_pixel is 16. */
	vncscr = rfbGetScreen(&argc, argv, scrinfo.xres, scrinfo.yres, 5, 0, (scrinfo.bits_per_pixel / 8));
	assert(vncscr != NULL);

	vncscr->desktopName = "Android";
	vncscr->frameBuffer = (char *)vncbuf;
	vncscr->alwaysShared = TRUE;
	vncscr->httpDir = NULL;
	vncscr->port = VNC_PORT;

	vncscr->kbdAddEvent = keyevent;
	vncscr->ptrAddEvent = ptrevent;
    vncscr->newClientHook = clientHook;

	rfbInitServer(vncscr);

	/* Mark as dirty since we haven't sent any updates at all yet. */
	rfbMarkRectAsModified(vncscr, 0, 0, scrinfo.xres, scrinfo.yres);

	/* No idea. */
	varblock.r_offset = scrinfo.red.offset + scrinfo.red.length - 5;
	varblock.g_offset = scrinfo.green.offset + scrinfo.green.length - 5;
	varblock.b_offset = scrinfo.blue.offset + scrinfo.blue.length - 5;
	varblock.rfb_xres = scrinfo.yres;
	varblock.rfb_maxy = scrinfo.xres - 1;
}

/*****************************************************************************/
void injectKeyEvent(uint16_t code, uint16_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time,0);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }

    printf("injectKey (%d, %d)\n", code , value);    
}

void injectSyncEvent(int code)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time,0);
    ev.type = EV_SYN;
    ev.code = code;
    ev.value = 0;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write sync event failed, %s\n", strerror(errno));
    }

    printf("injectSyncEvent %d\n", code);    
}

void injectMiscEvent(int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time,0);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write sync event failed, %s\n", strerror(errno));
    }

    printf("injectSyncEvent %d %d %d\n", type, code, value);    
}


static int keysym2scancode(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
    int scancode = 0;

    int code = (int)key;
    if (code>='1' && code<='9') {
        scancode = (code - '1') + KEY_1;
    } else if (code == '0') {
        scancode = 11;
    } else if (code>=0xFF50 && code<=0xFF58) {
        static const uint16_t map[] =
             {  KEY_HOME, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
                KEY_HOME, KEY_LEFT, /*KEY_SOFT1, KEY_SOFT2,*/ KEY_END, 0 };
        scancode = map[code & 0xF];
        } else if (code>=0xFFE1 && code<=0xFFEE) {
    
        static const uint16_t map[] =
             {  KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                KEY_COMPOSE, KEY_COMPOSE,
                KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                0,0,
                KEY_LEFTALT, KEY_RIGHTALT,
                0, 0, 0, 0 };
        scancode = map[code & 0xF];
    } else if ((code>='A' && code<='Z') || (code>='a' && code<='z')) {
        static const uint16_t map[] = {
                KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
                KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
                KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
                KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
                KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
        scancode = map[(code & 0x5F) - 'A'];
    } else {
        switch (code) {
//            case 0x0003:    scancode = KEY_CENTER;      break;
            case 0x0020:    scancode = KEY_SPACE;       break;
//            case 0x0023:    scancode = KEY_SHARP;       break;
//            case 0x0033:    scancode = KEY_SHARP;       break;
            case 0x002C:    scancode = KEY_COMMA;       break;
            case 0x003C:    scancode = KEY_COMMA;       break;
            case 0x002E:    scancode = KEY_DOT;         break;
            case 0x003E:    scancode = KEY_DOT;         break;
            case 0x002F:    scancode = KEY_SLASH;       break;
            case 0x003F:    scancode = KEY_SLASH;       break;
            case 0x0032:    scancode = KEY_EMAIL;       break;
            case 0x0040:    scancode = KEY_EMAIL;       break;
            case 0xFF08:    scancode = KEY_BACKSPACE;   break;
            case 0xFF1B:    scancode = KEY_BACK;        break;
            case 0xFF09:    scancode = KEY_TAB;         break;
            case 0xFF0D:    scancode = KEY_ENTER;       break;
//            case 0x002A:    scancode = KEY_STAR;        break;
            case 0xFFBE:    scancode = KEY_F1;        break; // F1
            case 0xFFBF:    scancode = KEY_F2;         break; // F2
            case 0xFFC0:    scancode = KEY_F3;        break; // F3
            case 0xFFC5:    scancode = KEY_F4;       break; // F8
            case 0xFFC8:    rfbShutdownServer(cl->screen,TRUE);       break; // F11            
        }
    }

    return scancode;
}

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
	int scancode;

    if (down)
    {
        if ((scancode = keysym2scancode(down, key, cl)))
        {
            printf("Got keysym: %04x (down=%d) scancode=%d\n", (unsigned int)key, (int)down, scancode);
            injectKeyEvent(scancode, down);
            injectKeyEvent(scancode, 0);
            injectSyncEvent(SYN_REPORT);
        }
    }
}

void injectTouchEvent(int down)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time,0);
    printf("touch=%d\n", down);
    #if 1
    ev.type = EV_KEY;
    ev.code = BTN_TOUCH;
    ev.value = down;
    #else
    ev.type = EV_ABS;
    ev.code = ABS_MT_PRESSURE;
    if (down)
    {
        ev.value = 494;
    } else {
        ev.value = 0;
    }
    #endif
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }
    ev.type = EV_KEY;
    ev.code = BTN_LEFT;
    ev.value = down;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }
}

void injectMouseEvent(int x, int y) {
    struct input_event ev;
    printf("mouse %d %d\n", x, y);
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_ABS;
    #if 1
    ev.code = ABS_X;
    #else
    ev.code = ABS_MT_POSITION_X;
    #endif
    ev.value = x;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }
    gettimeofday(&ev.time,0);
    ev.type = EV_ABS;
    #if 1
    ev.code = ABS_Y;
    #else
    ev.code = ABS_MT_POSITION_Y;
    #endif
    ev.value = y;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }
}

#if 0
    // Calculate the final x and y
    /* Fake touch screen always reports zero */
    if (xmin != 0 && xmax != 0 && ymin != 0 && ymax != 0)
    {
        x = xmin + (x * (xmax - xmin)) / (scrinfo.xres);
        y = ymin + (y * (ymax - ymin)) / (scrinfo.yres);
    // Then send a BTN_TOUCH

    // Then send the X
    gettimeofday(&ev.time,0);
    ev.type = EV_ABS;
    ev.code = ABS_X;
    ev.value = x;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }

    // Then send the Y
    gettimeofday(&ev.time,0);
    ev.type = EV_ABS;
    ev.code = ABS_Y;
    ev.value = y;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }

    // Finally send the SYN
    gettimeofday(&ev.time,0);
    ev.type = EV_SYN;
    ev.code = 0;
    ev.value = 0;
    if(write(uinputfd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }
#endif



static void ptrevent(int buttonMask, int _x, int _y, rfbClientPtr cl)
{
	/* Indicates either pointer movement or a pointer button press or release. The pointer is
now at (x-position, y-position), and the current state of buttons 1 to 8 are represented
by bits 0 to 7 of button-mask respectively, 0 meaning up, 1 meaning down (pressed).
On a conventional mouse, buttons 1, 2 and 3 correspond to the left, middle and right
buttons on the mouse. On a wheel mouse, each step of the wheel upwards is represented
by a press and release of button 4, and each step downwards is represented by
a press and release of button 5. 
  From: http://www.vislab.usyd.edu.au/blogs/index.php/2009/05/22/an-headerless-indexed-protocol-for-input-1?blog=61 */
    static int leftPressed = 0;
    static int rightPressed = 0;
    int x, y;
    
    x = (_x * 0x7fff) / cl->screen->width;
    y = (_y * 0x7fff) / cl->screen->height;

    if(buttonMask & 1) {
        printf("_x=%X _y=%X w=%X H=%X x=%x y=%x\n", _x, _y, cl->screen->width, cl->screen->height, x, y);
        if(!leftPressed) {
            leftPressed = 1;
            //printf("injectTouchEvent (x=%d, y=%d, down=%d)\n", x , y, buttonMask);    
//            printf("%d %d\n", x, y);
            injectMouseEvent(x, y);
            injectTouchEvent(1);
//            injectMiscEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, 1);
//            injectMiscEvent(EV_ABS, ABS_MT_TRACKING_ID, 0);
//            injectSyncEvent(SYN_MT_REPORT);
            injectSyncEvent(SYN_REPORT);
        } else {
            injectMouseEvent(x, y);
//            injectMiscEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, 1);
//            injectMiscEvent(EV_ABS, ABS_MT_TRACKING_ID, 0);
//            injectSyncEvent(SYN_MT_REPORT);
            injectSyncEvent(SYN_REPORT);
        }
    } else {
        if (leftPressed)
        {
            injectMouseEvent(x, y);
            injectTouchEvent(0);
//            injectMiscEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, 1);
//            injectMiscEvent(EV_ABS, ABS_MT_TRACKING_ID, 0);
//            injectSyncEvent(SYN_MT_REPORT);
            injectSyncEvent(SYN_REPORT);
            leftPressed = 0;
        }
    }

}

#define PIXEL_FB_TO_RFB(p,r,g,b) ((p>>r)&0x1f001f)|(((p>>g)&0x1f001f)<<5)|(((p>>b)&0x1f001f)<<10)

static int update_screen(void)
{
	unsigned int *f, *c, *r;
	int x, y;

	varblock.min_i = varblock.min_j = 9999;
	varblock.max_i = varblock.max_j = -1;

	f = (unsigned int *)fbmmap;        /* -> framebuffer         */
	c = (unsigned int *)fbbuf;         /* -> compare framebuffer */
	r = (unsigned int *)vncbuf;        /* -> remote framebuffer  */

	for (y = 0; y < scrinfo.yres; y++)
	{
		/* Compare every 2 pixels at a time, assuming that changes are likely
		 * in pairs. */
		for (x = 0; x < scrinfo.xres; x += 2)
		{
			unsigned int pixel = *f;

			if (pixel != *c)
			{
				*c = pixel;

				/* XXX: Undo the checkered pattern to test the efficiency
				 * gain using hextile encoding. */
				//if (pixel == 0x18e320e4 || pixel == 0x20e418e3)
				//	pixel = 0x18e318e3;

				*r = PIXEL_FB_TO_RFB(pixel,
				  varblock.r_offset, varblock.g_offset, varblock.b_offset);

				if (x < varblock.min_i)
					varblock.min_i = x;
				if (x > varblock.max_i)
						varblock.max_i = x;

					if (y > varblock.max_j)
						varblock.max_j = y;
					if (y < varblock.min_j)
						varblock.min_j = y;
				
			}

			f++, c++;
			r++;
		}
	}

	if (varblock.min_i < 9999)
	{
		if (varblock.max_i < 0)
			varblock.max_i = varblock.min_i;

		if (varblock.max_j < 0)
			varblock.max_j = varblock.min_j;

		fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
		  (varblock.max_i+2) - varblock.min_i, (varblock.max_j+1) - varblock.min_j,
		  varblock.min_i, varblock.min_j);

		rfbMarkRectAsModified(vncscr, varblock.min_i, varblock.min_j,
		  varblock.max_i + 2, varblock.max_j + 1);
        return 1;
	}
    return 0;
}

/*****************************************************************************/

void print_usage(char **argv)
{
	printf("%s [-h]\n"
		"-h : print this help\n", argv[0]);
}

uint32_t elapsedTimeMsec(const struct timeval* start) {
    struct timeval now;
    uint32_t elapsedMsec;

    gettimeofday(&now, NULL);
    elapsedMsec = (now.tv_sec - start->tv_sec) * 1000;  // sec to msec
    elapsedMsec += elapsedMsec + (now.tv_usec / 1000) - (start->tv_usec / 1000);
    return elapsedMsec;
}

uint32_t elapsedTimeUsec(const struct timeval* start) {
    struct timeval now;
    uint32_t elapsedUsec;

    gettimeofday(&now, NULL);
    elapsedUsec = (now.tv_sec - start->tv_sec) * 1000000;  // sec to Usec
    elapsedUsec = elapsedUsec + now.tv_usec - start->tv_usec;
    return elapsedUsec;
}

int main(int argc, char **argv)
{
	if(argc > 1)
	{
		int i=1;
		while(i < argc)
		{
			if(*argv[i] == '-')
			{
				switch(*(argv[i] + 1))
				{
					case 'h':
						print_usage(argv);
						exit(0);
						break;
				}
			}
			i++;
		}
	}

	printf("Initializing framebuffer device " FB_DEVICE "...\n");
	init_fb();
	printf("Initializing uinput device ...\n");
	if(init_uinput() != 0)
      goto main_fail_1;

	printf("Initializing VNC server:\n");
	printf("	width:  %d\n", (int)scrinfo.xres);
	printf("	height: %d\n", (int)scrinfo.yres);
	printf("	bpp:    %d\n", (int)scrinfo.bits_per_pixel);
	printf("	port:   %d\n", (int)VNC_PORT);
	init_fb_server(argc, argv);

    {
        struct timeval start;
        gettimeofday(&start, NULL);
        uint32_t delay = 10;

        /* Implement our own event loop to detect changes in the framebuffer. */
        while (1)
        {
            do {
                uint32_t elapsed = elapsedTimeMsec(&start);
                if (elapsed < delay)
                {
                    rfbProcessEvents(vncscr, (delay - elapsed) * 1000);
                } else {
                    break;
                }
            } while(1);

            if (activeClients)
            {
                int clean;
                gettimeofday(&start, NULL);
                clean = update_screen();
                printf("scr delay=%d  elapsedUsec=%d\n", delay, elapsedTimeUsec(&start));
                if(!update_screen()) {
                    if (delay < 1000)
                    {
                        delay += 50;
                    }
                } else {
                    delay = 100;
                }
            } else {
                delay = 500;
            }
            gettimeofday(&start, NULL);
        }
    }

    main_fail_1:
	printf("Cleaning up...\n");
	cleanup_fb();
	cleanup_uinput();
}

