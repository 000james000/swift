typedef const void *CFTypeRef;
CFTypeRef CFBottom();

typedef const struct __CCPowerSupply *CCPowerSupplyRef;
typedef const struct __CCItem *CCItemRef;

/// The standard power supply.
extern const CCPowerSupplyRef kCCPowerStandard;

typedef const struct __CCRefrigerator *CCRefrigeratorRef;
CCRefrigeratorRef CCRefrigeratorCreate(CCPowerSupplyRef power);

void CCRefrigeratorOpen(CCRefrigeratorRef fridge);
CCItemRef CCRefrigeratorGet(CCRefrigeratorRef fridge, unsigned index);
void CCRefrigeratorClose(CCRefrigeratorRef fridge);

typedef struct __CCRefrigerator *CCMutableRefrigeratorRef;
CCMutableRefrigeratorRef CCRefrigeratorCreateMutable(CCPowerSupplyRef power);

void CCRefrigeratorInsert(CCMutableRefrigeratorRef fridge, CCItemRef ref);

@interface Kitchen
@property CCRefrigeratorRef fridge;
@end

@interface Duct
@end

@interface MutableDuct : Duct
@end

typedef const struct __attribute__((objc_bridge(Duct))) __CCDuct *CCDuctRef;
typedef struct __attribute__((objc_bridge_mutable(MutableDuct))) __CCDuct *CCMutableDuctRef;

typedef CCRefrigeratorRef CCFridgeRef;
