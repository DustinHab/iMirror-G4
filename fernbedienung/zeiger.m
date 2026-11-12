/* Zeiger: grosser runder Mauszeiger fuer den Mausmodus der Fernbedienung.
   Wird von fernbedienung gestartet und beendet; laeuft nur, solange der Mausmodus an ist.
   Ein durchsichtiges Kreisfenster ueber allem folgt dem Zeiger (reine Lausch-Ereignisschleuse),
   mit Punkt in der Mitte; der Pfeil selbst wird ausgeblendet. */
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <unistd.h>

#define D 44.0

static NSWindow *Fenster;
static CGFloat Hoehe;
static CFMachPortRef Schleuse;

@interface Kreis : NSView
@end

@implementation Kreis
- (BOOL)isOpaque { return NO; }
- (void)drawRect:(NSRect)r
{
    NSRect k = NSInsetRect([self bounds], 3, 3);
    NSBezierPath *p;
    [[NSColor clearColor] set];
    NSRectFill(r);
    p = [NSBezierPath bezierPathWithOvalInRect:k];
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.30] set];
    [p fill];
    [p setLineWidth:3.0];
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.85] set];
    [p stroke];
    p = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(k, 2.5, 2.5)];
    [p setLineWidth:1.5];
    [[NSColor whiteColor] set];
    [p stroke];
    /* Klickpunkt: kleiner schwarzer Punkt mit feinem weissem Rand */
    p = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(NSMidX([self bounds]) - 3, NSMidY([self bounds]) - 3, 6, 6)];
    [[NSColor blackColor] set];
    [p fill];
    [p setLineWidth:1.0];
    [[NSColor whiteColor] set];
    [p stroke];
}
@end

static NSCursor *Leer;   /* durchsichtiger Zeiger statt Pfeil */
static id Waechter;
static pid_t Eltern;     /* fernbedienung: nur deren Ereignisse gehoeren zum Mausmodus */
static int Signalrohr[2];

static void hinsetzen(CGPoint p)
{
    [Fenster setFrameOrigin:NSMakePoint(floor(p.x - D / 2), floor(Hoehe - p.y - D / 2))];
}

/* normalen Pfeil zurueck, Kreis weg, Ende */
static void aufraeumen(void)
{
    [NSObject cancelPreviousPerformRequestsWithTarget:Waechter];
    [Fenster orderOut:nil];
    CGDisplayShowCursor(kCGDirectMainDisplay);
    [[NSCursor arrowCursor] set];
    exit(0);
}

static void beenden(CFSocketRef s, CFSocketCallBackType t, CFDataRef a, const void *d, void *i)
{
    aufraeumen();
}

static void signal_ab(int s)
{
    char c = 1;
    write(Signalrohr[1], &c, 1);
}

/* Das vordere Programm setzt seinen Zeiger bei Bewegungen neu; danach wieder leeren */
static void verstecken(void)
{
    [Leer set];
    [NSObject cancelPreviousPerformRequestsWithTarget:Waechter selector:@selector(nachsetzen:) object:nil];
    [Waechter performSelector:@selector(nachsetzen:) withObject:nil afterDelay:0.04];
}

static CGEventRef lauscher(CGEventTapProxy pr, CGEventType t, CGEventRef e, void *i)
{
    (void)pr; (void)i;
    if (t == kCGEventTapDisabledByTimeout || t == kCGEventTapDisabledByUserInput) {
        if (Schleuse) CGEventTapEnable(Schleuse, true);
        return e;
    }
    /* Trackpad oder Maus am iBook selbst: sofort wieder normaler Pfeil */
    if ((pid_t)CGEventGetIntegerValueField(e, kCGEventSourceUnixProcessID) != Eltern) aufraeumen();
    if (t == kCGEventLeftMouseDown || t == kCGEventRightMouseDown || t == kCGEventScrollWheel) return e;
    hinsetzen(CGEventGetLocation(e));
    verstecken();
    return e;
}

@interface Wache : NSObject
@end

@implementation Wache
- (void)nachsetzen:(id)x
{
    [Leer set];
}
- (void)pruefen:(NSTimer *)t
{
    if (getppid() != Eltern) aufraeumen();   /* fernbedienung ist weg */
}
- (void)nachfuehren:(NSTimer *)t
{
    CGEventRef e = CGEventCreate(NULL);
    if (e) { hinsetzen(CGEventGetLocation(e)); CFRelease(e); }
}
@end

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    Wache *w;
    CGEventRef e;
    CGEventMask maske = CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged)
                      | CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged)
                      | CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventRightMouseDown)
                      | CGEventMaskBit(kCGEventScrollWheel);
    int (*verbindung)(void) = dlsym(RTLD_DEFAULT, "_CGSDefaultConnection");
    CGError (*eigenschaft)(int, int, CFStringRef, CFTypeRef) = dlsym(RTLD_DEFAULT, "CGSSetConnectionProperty");

    Eltern = getppid();
    [NSApplication sharedApplication];
    /* Beenden durch fernbedienung (SIGTERM): erst den Pfeil zurueckgeben */
    pipe(Signalrohr);
    {
        CFSocketRef s = CFSocketCreateWithNative(NULL, Signalrohr[0], kCFSocketReadCallBack, beenden, NULL);
        CFRunLoopSourceRef q = CFSocketCreateRunLoopSource(NULL, s, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), q, kCFRunLoopCommonModes);
    }
    signal(SIGTERM, signal_ab);
    Hoehe = [[[NSScreen screens] objectAtIndex:0] frame].size.height;
    Fenster = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, D, D) styleMask:NSBorderlessWindowMask
                                            backing:NSBackingStoreBuffered defer:NO];
    [Fenster setOpaque:NO];
    [Fenster setBackgroundColor:[NSColor clearColor]];
    [Fenster setHasShadow:NO];
    [Fenster setIgnoresMouseEvents:YES];
    [Fenster setLevel:CGWindowLevelForKey(kCGCursorWindowLevelKey) - 1];
    if ([Fenster respondsToSelector:@selector(setCollectionBehavior:)]) [Fenster setCollectionBehavior:1];   /* auf allen Spaces */
    [Fenster setContentView:[[[Kreis alloc] initWithFrame:NSMakeRect(0, 0, D, D)] autorelease]];
    e = CGEventCreate(NULL);
    if (e) { hinsetzen(CGEventGetLocation(e)); CFRelease(e); }
    [Fenster orderFrontRegardless];

    w = [[Wache alloc] init];
    Waechter = w;
    {
        NSImage *bild = [[[NSImage alloc] initWithSize:NSMakeSize(16, 16)] autorelease];
        [bild lockFocus];
        [[NSColor clearColor] set];
        NSRectFill(NSMakeRect(0, 0, 16, 16));
        [bild unlockFocus];
        Leer = [[NSCursor alloc] initWithImage:bild hotSpot:NSMakePoint(0, 0)];
    }
    Schleuse = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, maske, lauscher, NULL);
    if (Schleuse) {
        CFRunLoopSourceRef q = CFMachPortCreateRunLoopSource(NULL, Schleuse, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), q, kCFRunLoopCommonModes);
        CFRelease(q);
    } else {
        [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:w selector:@selector(nachfuehren:) userInfo:nil repeats:YES];
    }
    [NSTimer scheduledTimerWithTimeInterval:2.0 target:w selector:@selector(pruefen:) userInfo:nil repeats:YES];

    /* Pfeil ausblenden, auch wenn ein anderes Programm vorn ist (private Verbindungseigenschaft) */
    if (verbindung && eigenschaft) eigenschaft(verbindung(), verbindung(), CFSTR("SetsCursorInBackground"), kCFBooleanTrue);
    CGDisplayHideCursor(kCGDirectMainDisplay);
    verstecken();

    [NSApp run];
    [pool release];
    return 0;
}
