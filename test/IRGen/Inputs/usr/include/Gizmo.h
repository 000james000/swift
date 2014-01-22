@import ObjectiveC;
@import Foundation;

#define NS_RETURNS_RETAINED __attribute__((ns_returns_retained))
#define NS_CONSUMES_SELF __attribute__((ns_consumes_self))
#define NS_CONSUMED __attribute__((ns_consumed))

struct Rect {
  float x;
  float y;
  float width;
  float height;
};

struct NSRect {
  struct NSPoint {
    double x;
    double y;
  } origin;
  struct NSSize {
    double width;
    double height;
  } size;
};

struct Trio {
  double i;
  double j;
  double k;
};

struct IntPair {
  int a;
  int b;
};

struct NestedInts {
  struct A {
    int value;
  } a;
  struct B {
    int value;
  } b;
};

typedef long NSInteger;

#if !defined(OBJC_HIDE_64) && TARGET_OS_IPHONE && __LP64__
typedef bool BOOL;
#else
typedef signed char BOOL;
// BOOL is explicitly signed so @encode(BOOL) == "c" rather than "C"
// even if -funsigned-char is used.
#endif

@interface Gizmo : NSObject
- (Gizmo*) clone NS_RETURNS_RETAINED;
- (Gizmo*) duplicate;
- (Gizmo*) initWithBellsOn:(NSInteger)x;
- (void) fork NS_CONSUMES_SELF;
- (void) enumerateSubGizmos: (void (^)(Gizmo*))f;
+ (void) consume: (NS_CONSUMED Gizmo*) gizmo;
+ (void) inspect: (Gizmo*) gizmo;
+ (void) runWithRect: (struct Rect) rect andGizmo: (Gizmo*) gizmo;
- (struct NSRect) frame;
- (void) setFrame: (struct NSRect) rect;
- (void) frob;
+ (void) runce;
- (BOOL) negate:(BOOL) b;
@end

@interface NSView : NSObject
- (struct NSRect) convertRectFromBase: (struct NSRect) r;
@end

struct NSRect NSMakeRect(double, double, double, double);
struct NSRect NSInsetRect(struct NSRect, double, double);
NSString *NSStringFromRect(struct NSRect r);

@protocol NSRuncing
- (void)runce;
- (void)foo;
@end

@protocol NSFunging
- (void)funge;
- (void)foo;
@end

@interface NSSpoon <NSRuncing, NSFunging>
- (void)runce;
- (void)funge;
- (void)foo;
@end

@interface NSStructReturns
- (struct Rect)newRect;
- (struct Trio)newTrio;
- (struct IntPair)newPair;
- (struct NestedInts)newNestedInts;
@end

typedef NS_ENUM(unsigned short, NSRuncingOptions) {
  NSRuncingMince = 123,
  NSRuncingQuinceSliced = 4567,
  NSRuncingQuinceJulienned = 5678,
  NSRuncingQuinceDiced = 6789,
};

typedef NS_ENUM(int, NSRadixedOptions) {
  NSRadixedOctal = 0755,
  NSRadixedHex = 0xFFFF,
};

typedef NS_ENUM(int, NSNegativeOptions) {
  NSNegativeFoo = -1,
  NSNegativeBar = -0x7FFFFFFF - 1,
};

typedef NS_ENUM(unsigned, NSNegativeUnsignedOptions) {
  NSNegativeUnsignedFoo = -1,
  NSNegativeUnsignedBar = -0x7FFFFFFF - 1,
};
