@import Foundation;

@protocol NSAppearanceCustomization <NSObject>
@end

@interface NSResponder : NSObject
@end

@protocol NSAccessibilityElement <NSObject>
@required
- (id)accessibilityParent;
@end
@protocol NSAccessibilityButton <NSAccessibilityElement>
@required
- (NSString *)accessibilityLabel;
- (BOOL)accessibilityPerformPress;
@end
// The complete accessibility protocol
@protocol NSAccessibility <NSObject>
@required
// Element containing this UIElement
// Invokes when clients request NSAccessibilityParentAttribute
@property (weak) id accessibilityParent;
// Description of UIElement
// Invokes when clients request NSAccessibilityDescriptionAttribute
@property (copy) NSString *accessibilityLabel;
- (NSString *)accessibilityLabel;
// Invokes when clients perform NSAccessibilityPressAction
- (BOOL)accessibilityPerformPress;
@property (getter = isAccessibilityFocused) BOOL accessibilityFocused;
@end

// Specifically testing re-adopting a protocol that's adopted by a base class.
@interface NSWindow : NSResponder <NSAppearanceCustomization>
@end

@interface NSDocument : NSObject
- (instancetype)init;
- (instancetype)initWithContentsOfURL:(NSURL *)url ofType:(NSString *)type error:(NSError **)error;

- (BOOL)readFromURL:(NSURL *)url ofType:(NSString *)typeName error:(NSError **)outError;
- (BOOL)writeToURL:(NSURL *)url ofType:(NSString *)typeName error:(NSError **)outError;

- (void)copyDocumentFromURL:(NSURL*)fromURL toURL:(NSURL*)toURL;
- (void)scaleXBy:(NSInteger)value;
@end

@interface NSDocument (NSDeprecated)
- (nullable id)initWithContentsOfURL:(NSURL *)url ofType:(NSString *)typeName __attribute__((availability(macosx, introduced=10.0, deprecated=10.4)));
- (BOOL)readFromURL:(NSURL *)url ofType:(NSString *)type __attribute__((availability(macosx, introduced=10.0, deprecated=10.4)));
- (BOOL)writeToURL:(NSURL *)url ofType:(NSString *)type __attribute__((availability(macosx, introduced=10.0, deprecated=10.4)));
@end

@interface NSAwesomeDocument : NSDocument
-(void)noReturnMethod:(int)arg  __attribute__((noreturn));
@end

@interface URLDocument : NSObject
- (instancetype)init NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithURL:(NSString *)urlString;
@end

@interface NSInterestingDesignated : NSObject
+ (instancetype)alloc;
- (instancetype)initWithString:(NSString*)str NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithURL:(NSString*)str;
@end

@interface NSInterestingDesignatedSub : NSInterestingDesignated
@end

@interface NSColor : NSObject
+ (instancetype)colorWithDeviceRed:(double)red green:(double)green blue:(double)blue alpha:(double)alpha;
@end

@protocol NSAnimatablePropertyContainer
- (instancetype)animator __attribute__((availability(macosx,introduced=10.5)));
@end

@interface NSLayoutConstraint : NSObject <NSAnimatablePropertyContainer>
@property double constant;
@end

@protocol MyDelegate
  -(BOOL)receiverShouldJumpOnTable:(NSObject *)table;
@end

@interface NSScrollView : NSObject
- (void)scrollItemAtIndexToTop:(int)index;
@end

@interface NSViewController : NSObject <NSCoding>
@end

@interface NSTableViewController : NSViewController
-(instancetype)initWithInt:(NSInteger)value NS_DESIGNATED_INITIALIZER;
@end

@interface NSObjectFactory : NSObject
+(instancetype)objectFactory;
+(instancetype)objectFactoryWithInteger:(NSInteger)i;
+(instancetype)factoryWithDouble:(double)i;
+(id)factoryWithString:(NSString *)s;
+(NSObjectFactory*)factoryWithFloat:(float)f;
+(instancetype)factoryBuildingWidgets;
@end

@interface NSObjectFactorySub : NSObjectFactory
@end

@interface NSString(Category)
- (NSString*)nsStringMethod;
+ (NSInteger)nsStringClassMethod;
@property (readonly) NSString *nsStringProperty;
+ (NSString*)someFactoryMethod;
@end

@interface NSDictionary(Category)
- (NSDictionary*)nsDictionaryMethod;
+ (NSInteger)nsDictionaryClassMethod;
@property (readonly) NSDictionary *nsDictionaryProperty;
@end

@interface CALayer
- (instancetype)init;
@end

@interface NSView : NSObject <NSCoding, NSAccessibility>
- (nullable instancetype)initWithCoder:(nonnull NSCoder *)aDecoder;
- (BOOL)isDescendantOf:(nonnull NSView *)aView;
- (nullable NSView *)ancestorSharedWithView:(nonnull NSView *)aView;
- (void)setSubviews:(nonnull NSArray *)newSubviews;
- (void)addSubview:(nonnull NSView *)aView;
- (void)addSubview:(nonnull NSView *)aView positioned:(unsigned)place relativeTo:(nullable NSView *)otherView;
@property (readonly, assign, nullable) NSView *superview;
@property (strong, nullable) CALayer *layer;
@property (readonly, copy, nonnull) NSArray *trackingAreas;
@property (copy, nonnull) NSArray *subviews;
@end

@interface NSView(NSKeyboardUI)
@property (assign, nullable) NSView *nextKeyView;
@end

@interface NSMenu : NSObject <NSCopying, NSCoding>
- (instancetype)initWithTitle:(NSString *)title;
@end

@interface NSMenuItem : NSObject <NSCopying, NSCoding>
// Setter is only for subclassers.
@property (assign, nullable) NSMenu *menu;

@property (copy, nonnull) NSString *title;
@property (copy, nullable) NSAttributedString *attributedTitle;

@property (weak) id target;
@property SEL action;
@end

extern NSString *NSViewFrameDidChangeNotification;
extern NSString *NSViewFocusDidChangeNotification;

@protocol NSApplicationDelegate
@end


