/* Mercury for iOS: the app that hosts the modem.
 *
 * Copyright (C) 2026 Rhizomatica
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * Mercury itself is unchanged: main.c is compiled with its main() renamed to
 * mercury_main(), and this app runs it on a thread of its own, with the app's
 * launch arguments, while UIKit keeps the main thread.  Everything else --
 * the TNC ports, the WebSocket UI, the modem -- is the daemon as on any other
 * platform.  The app exists because iOS gives the microphone, and keeps
 * running in the background, only for an app: a bundle with a usage string
 * and the "audio" background mode (Info.plist.in).
 */

#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

extern int mercury_main(int argc, char *argv[]);

static int g_argc;
static char **g_argv;

static void *mercury_thread(void *unused)
{
	int rc = mercury_main(g_argc, g_argv);
	fprintf(stderr, "mercury exited (%d)\n", rc);
	fflush(stderr);
	exit(rc);
	return NULL;
}

@interface MercuryAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation MercuryAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	UIViewController *vc = [UIViewController new];
	vc.view.backgroundColor = UIColor.systemBackgroundColor;
	UILabel *label = [[UILabel alloc] initWithFrame:vc.view.bounds];
	label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	label.textAlignment = NSTextAlignmentCenter;
	label.numberOfLines = 0;
	label.text = @"Mercury\n\nTNC on ports 8300/8301\nUI on port 10000";
	[vc.view addSubview:label];
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];

	/* 8 MB, as the modem's own threads get (modem.c): the wide OFDM modes put
	 * large buffers on the stack, more than a default secondary thread has. */
	pthread_attr_t attr;
	pthread_t t;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
	pthread_create(&t, &attr, mercury_thread, NULL);
	pthread_attr_destroy(&attr);
	return YES;
}

@end

int main(int argc, char *argv[])
{
	g_argc = argc;
	g_argv = argv;
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([MercuryAppDelegate class]));
	}
}
