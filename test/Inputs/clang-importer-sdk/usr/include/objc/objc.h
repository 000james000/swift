typedef unsigned long NSUInteger;
typedef long NSInteger;

@protocol NSObject
- description;
@end

@interface NSObject <NSObject>
+ alloc;
- init;
+ new;
- performSelector:(SEL)selector withObject:(id)obj;
- (Class)myClass;
+ description;
- description;
@end

@interface A : NSObject
- (int)method:(int)arg withDouble:(double)d;
+ (int)classMethod;
+ (int)classMethod:(int)arg;
- (int)counter;
@property int counter;
- (void)setCounter:(int)value;

- (int)informalProp;

- (int)informalMadeFormal;
- init;

@property int overriddenProp;
@end

@protocol B
- (int)method:(int)arg withFloat:(float)f;
- (int)otherMethod:(int)arg withFloat:(float)f;
@end

@protocol Cat1
- cat1Method;
@end

@interface B : A <B>
- (int)method:(int)arg withFloat:(float)f;
+ (int)classMethod:(int)arg withInt:(int)i;
- (id<B>)getAsProto;
- (id<B, Cat1>)getAsProtoWithCat;
- performAdd:(int)x withValue:(int)y withValue:(int)z withValue2:(int)w;
@property (readonly) int readCounter;

@property int informalMadeFormal;

@property int overriddenProp;

- initWithInt:(int)i;
- initWithInt:(int)i andDouble:(double)d;
- initWithDouble:(double)d1 :(double)d2;
- initBBB:(B*)b;
- initForWorldDomination;
- notAnInit __attribute__((objc_method_family(init), ns_returns_retained));
- (id)_initFoo;
- (void)anotherMethodOnB;

+ (void)instanceTakesObjectClassTakesFloat:(float)x;
- (void)instanceTakesObjectClassTakesFloat:(id)x;

@end

@interface A(Cat1) <Cat1>
- method:(int)i onCat1:(double)d;
- cat1Method;
@end

@interface A()
- method:(int)i onExtA:(double)d;
@end

@interface B()
- method:(int)i onExtB:(double)d;
+ newWithA:(A*)a;
@end

@interface A(Subscripting)
- objectAtIndexedSubscript:(NSInteger)idx;
- (void)setObject:(id)object atIndexedSubscript:(NSInteger)idx;

- objectForKeyedSubscript:(id)key;
@end

@interface B(Subscripting)
- (void)setObject:(id)object forKeyedSubscript:(id)key;
@end

@protocol P2
- (void)p2Method;
- (id)initViaP2:(double)x second:(double)y;
@end

@interface B(P2) <P2>
@end

@interface NSDate : NSObject
- (signed char)isEqualToDate:(NSDate *)anotherDate;
@end

NSDate *swift_createDate(void);

@interface NSProxy
+ alloc;
@end

@interface AProxy : NSProxy
- initWithInt:(int)i;
@end

typedef signed char BOOL;

@interface A(BoolStuff)
- setEnabled:(BOOL)enabled;
@end

typedef struct objc_selector    *SEL;

@interface AlmostSubscriptable
- (A*) objectForKeyedSubscript:(id)key;
- (void)setObject:(id)object forKeyedSubscript:(id)key;
@end
