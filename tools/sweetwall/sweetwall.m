/*
 * sweetwall (ios-sweet plan Stage 7.7): the layered, animated iOS 16 wallpaper
 * on the legacy PaperBoardUI path. Our own code.
 *
 * With Autobahn (posters) off, SpringBoard draws the lock and home wallpapers
 * as PBUIWallpaperView subclasses showing a CPBitmap, and a LayeredAnimation
 * wallpaper (three Core Animation packages with Locked / Unlock / Sleep states)
 * has nothing on that path to play it. Loaded into SpringBoard with
 * DYLD_INSERT_LIBRARIES (sweetenv), this library:
 *
 *   - wraps -[PBUIWallpaperView didMoveToWindow] / -layoutSubviews: every
 *     wallpaper view that reaches a window gets a host layer over its content
 *     view holding the packages' root layers, bottom to top
 *     SWEETWALL_DIR/{Background,Foreground,Floating}.ca, scaled to fill
 *   - loads each package as BaseBoardUI's BSUICAPackageView does (CAPackage
 *     CAML bundle, rootLayer.geometryFlipped = package.isGeometryFlipped, a
 *     CAStateController on the root layer)
 *   - follows com.apple.springboard.hasBlankedScreen and
 *     com.apple.springboard.lockstate: screen off -> Sleep, locked -> Locked,
 *     unlocked -> Unlock, animated with the packages' own transitions
 *
 * The packages are copies of the system's wallpaper with the HEIC assets as
 * PNG (tools/sunfish/sweetwall-pkg.py), so nothing needs a hardware decoder.
 * The CPBitmap wallpaper stays underneath. Built without ARC; logs to stderr.
 */
#include <math.h>
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <objc/message.h>
#include <objc/runtime.h>

#define SWEETWALL_DIR "/Library/Wallpaper/SweetLayered"
#define SWEETWALL_DOC_W 375.0
#define SWEETWALL_DOC_H 812.0
/* smaller wallpaper views (previews, thumbnails) are left alone */
#define SWEETWALL_MIN_W 200.0

static const char *const package_names[] = { "Background", "Foreground", "Floating" };
#define NPACKAGES (sizeof(package_names) / sizeof(package_names[0]))

/*
 * QuartzCore private API, declared here and looked up at run time so that
 * nothing private is linked; the value of kCAPackageTypeCAMLBundle
 */
#define PACKAGE_TYPE_CAML_BUNDLE @"com.apple.coreanimation-bundle"

@interface CAPackage : NSObject
+ (instancetype)packageWithContentsOfURL:(NSURL *)url type:(NSString *)type options:(NSDictionary *)options error:(NSError **)error;
- (CALayer *)rootLayer;
- (BOOL)isGeometryFlipped;
@end

@interface CAState : NSObject
- (NSString *)name;
@end

@interface CALayer (SweetwallStates)
- (CAState *)stateWithName:(NSString *)name;
@end

@interface CAStateController : NSObject
- (instancetype)initWithLayer:(CALayer *)layer;
- (void)setState:(CAState *)state ofLayer:(CALayer *)layer;
- (void)setState:(CAState *)state ofLayer:(CALayer *)layer transitionSpeed:(float)speed;
@end

#define say(...) do { fprintf(stderr, "sweetwall: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* One wallpaper view's packages */
@interface SweetwallHost : NSObject {
@public
	CALayer *layer;
	CAPackage *packages[NPACKAGES];
	CAStateController *controllers[NPACKAGES];
	UIView *view;	/* not retained; the host is the view's associated object */
}
@end

@implementation SweetwallHost
- (void)dealloc
{
	[layer removeFromSuperlayer];
	[layer release];
	for (unsigned i = 0; i < NPACKAGES; i++) {
		[controllers[i] release];
		[packages[i] release];
	}
	[super dealloc];
}
@end

static char host_key;
static NSHashTable *hosts;	/* weak: every live SweetwallHost */
static const char *current_state = "Locked";
static int blanked_token = NOTIFY_TOKEN_INVALID, lockstate_token = NOTIFY_TOKEN_INVALID;
static IMP orig_didMoveToWindow, orig_layoutSubviews;
static Class package_class, state_controller_class;

static void
host_set_state(SweetwallHost *h, const char *name, bool animated)
{
	NSString *s = [NSString stringWithUTF8String:name];

	for (unsigned i = 0; i < NPACKAGES; i++) {
		CALayer *root = [h->packages[i] rootLayer];
		CAState *state = [root stateWithName:s];

		if (!h->controllers[i] || !state)
			continue;
		if (animated)
			[h->controllers[i] setState:state ofLayer:root transitionSpeed:1.0f];
		else
			[h->controllers[i] setState:state ofLayer:root];
	}
}

static void
host_layout(SweetwallHost *h)
{
	CGRect b = h->view.bounds;
	CGFloat k = fmax(b.size.width / SWEETWALL_DOC_W, b.size.height / SWEETWALL_DOC_H);

	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	h->layer.frame = b;
	for (unsigned i = 0; i < NPACKAGES; i++) {
		CALayer *root = [h->packages[i] rootLayer];

		root.position = CGPointMake(CGRectGetMidX(b) - b.origin.x, CGRectGetMidY(b) - b.origin.y);
		root.transform = CATransform3DMakeScale(k, k, 1.0);
	}
	[CATransaction commit];
}

static SweetwallHost *
host_create(UIView *view)
{
	SweetwallHost *h = [[SweetwallHost alloc] init];
	unsigned loaded = 0;

	h->view = view;
	h->layer = [[CALayer alloc] init];
	h->layer.name = @"sweetwall";
	h->layer.masksToBounds = YES;
	for (unsigned i = 0; i < NPACKAGES; i++) {
		NSString *path = [NSString stringWithFormat:@SWEETWALL_DIR "/%s.ca", package_names[i]];
		NSError *err = nil;
		CAPackage *p = [package_class packageWithContentsOfURL:[NSURL fileURLWithPath:path isDirectory:YES]
		    type:PACKAGE_TYPE_CAML_BUNDLE options:nil error:&err];
		CALayer *root = [p rootLayer];

		if (!root) {
			say("%s: %s", path.UTF8String, err ? err.description.UTF8String : "no root layer");
			continue;
		}
		h->packages[i] = [p retain];
		root.geometryFlipped = [p isGeometryFlipped];
		h->controllers[i] = [(CAStateController *)[state_controller_class alloc] initWithLayer:root];
		[h->layer addSublayer:root];
		loaded++;
	}
	if (!loaded) {
		[h release];
		return nil;
	}
	host_set_state(h, current_state, false);
	return [h autorelease];
}

static void
attach(UIView *view)
{
	SweetwallHost *h = objc_getAssociatedObject(view, &host_key);
	UIView *content = nil;

	if (!view.window || h || view.bounds.size.width < SWEETWALL_MIN_W)
		return;
	h = host_create(view);
	if (!h)
		return;
	objc_setAssociatedObject(view, &host_key, h, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	[hosts addObject:h];
	if ([view respondsToSelector:@selector(contentView)])
		content = ((UIView *(*)(id, SEL))objc_msgSend)(view, @selector(contentView));
	if (content && content.layer.superlayer == view.layer)
		[view.layer insertSublayer:h->layer above:content.layer];
	else
		[view.layer insertSublayer:h->layer atIndex:0];
	host_layout(h);
	say("on %s %p (%.0fx%.0f), state %s, %lu views", object_getClassName(view), view,
	    view.bounds.size.width, view.bounds.size.height, current_state, (unsigned long)hosts.count);
}

static void
m_didMoveToWindow(UIView *self, SEL _cmd)
{
	((void (*)(id, SEL))orig_didMoveToWindow)(self, _cmd);
	attach(self);
}

static void
m_layoutSubviews(UIView *self, SEL _cmd)
{
	SweetwallHost *h;

	((void (*)(id, SEL))orig_layoutSubviews)(self, _cmd);
	attach(self);
	h = objc_getAssociatedObject(self, &host_key);
	if (h)
		host_layout(h);
}

static uint64_t
notify_value(int token)
{
	uint64_t v = 0;

	if (token != NOTIFY_TOKEN_INVALID)
		notify_get_state(token, &v);
	return v;
}

static void
update_state(bool animated)
{
	const char *s = notify_value(blanked_token) ? "Sleep" : notify_value(lockstate_token) ? "Locked" : "Unlock";

	if (!strcmp(s, current_state))
		return;
	say("%s -> %s", current_state, s);
	current_state = s;
	for (SweetwallHost *h in hosts)
		host_set_state(h, s, animated);
}

/* Wrap a method the class itself implements; NO when it has no own implementation */
static BOOL
wrap(Class cls, const char *sel, IMP imp, IMP *orig)
{
	unsigned n;
	Method *ms = class_copyMethodList(cls, &n);
	Method m = NULL;

	for (unsigned i = 0; i < n && !m; i++)
		if (!strcmp(sel_getName(method_getName(ms[i])), sel))
			m = ms[i];
	free(ms);
	if (!m)
		return NO;
	*orig = method_setImplementation(m, imp);
	return YES;
}

static void
install(int tries)
{
	Class cls = objc_getClass("PBUIWallpaperView");

	if (!cls) {
		if (tries == 0) {
			say("PBUIWallpaperView not found, giving up");
			return;
		}
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			install(tries - 1);
		});
		return;
	}
	package_class = objc_getClass("CAPackage");
	state_controller_class = objc_getClass("CAStateController");
	if (!package_class || !state_controller_class) {
		say("QuartzCore lacks CAPackage / CAStateController");
		return;
	}
	hosts = [[NSHashTable weakObjectsHashTable] retain];
	notify_register_dispatch("com.apple.springboard.hasBlankedScreen", &blanked_token, dispatch_get_main_queue(),
	    ^(int t) { (void)t; update_state(true); });
	notify_register_dispatch("com.apple.springboard.lockstate", &lockstate_token, dispatch_get_main_queue(),
	    ^(int t) { (void)t; update_state(true); });
	update_state(false);
	if (!wrap(cls, "didMoveToWindow", (IMP)m_didMoveToWindow, &orig_didMoveToWindow) ||
	    !wrap(cls, "layoutSubviews", (IMP)m_layoutSubviews, &orig_layoutSubviews)) {
		say("PBUIWallpaperView lacks didMoveToWindow / layoutSubviews");
		return;
	}
	say("installed (packages in " SWEETWALL_DIR ", state %s)", current_state);
}

__attribute__((constructor)) static void
sweetwall_load(void)
{
	if (strcmp(getprogname(), "SpringBoard"))
		return;
	dispatch_async(dispatch_get_main_queue(), ^{
		install(30);
	});
}
