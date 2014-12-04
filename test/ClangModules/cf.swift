// RUN: %swift -parse -verify -import-cf-types -I %S/Inputs/custom-modules -target x86_64-apple-macosx10.9 %s

import CoreCooling

func assertUnmanaged<T: AnyObject>(t: Unmanaged<T>) {}
func assertManaged<T: AnyObject>(t: T) {}

func test0(fridge: CCRefrigeratorRef) {
  assertManaged(fridge)
}

func test1(power: Unmanaged<CCPowerSupplyRef>) {
  assertUnmanaged(power)
  let fridge = CCRefrigeratorCreate(power) // expected-error {{'Unmanaged<CCPowerSupplyRef>' is not convertible to 'CCPowerSupply'}}
  assertUnmanaged(fridge)
}

func test2() {
  let fridge = CCRefrigeratorCreate(kCCPowerStandard)
  assertUnmanaged(fridge)
}

func test3(fridge: CCRefrigerator) {
  assertManaged(fridge)
}

func test4() {
  // FIXME: this should not require a type annotation
  let power: CCPowerSupply = kCCPowerStandard
  assertManaged(power)
  let fridge = CCRefrigeratorCreate(power)
  assertUnmanaged(fridge)
}

func test5() {
  let power: Unmanaged<CCPowerSupply> = .passUnretained(kCCPowerStandard)
  assertUnmanaged(power)
  let fridge = CCRefrigeratorCreate(power.takeUnretainedValue())
}

func test6() {
  let fridge = CCRefrigeratorCreate(nil)
  fridge?.release()
}

func test7() {
  let value = CFBottom()
  assertUnmanaged(value)
}

func test8(f: CCRefrigerator) {
  let v1: CFTypeRef = f
  let v2: AnyObject = f
}

func test9() {
  let fridge = CCRefrigeratorCreateMutable(kCCPowerStandard).takeRetainedValue()
  let constFridge: CCRefrigerator = fridge
  CCRefrigeratorOpen(fridge)
  let item = CCRefrigeratorGet(fridge, 0).takeUnretainedValue()
  CCRefrigeratorInsert(item, fridge) // expected-error {{'CCItem' is not convertible to 'CCMutableRefrigerator'}}
  CCRefrigeratorInsert(constFridge, item) // expected-error {{'CCRefrigerator' is not convertible to 'CCMutableRefrigerator'}}
  CCRefrigeratorInsert(fridge, item)
  CCRefrigeratorClose(fridge)
}

func testProperty(k: Kitchen) {
  k.fridge = CCRefrigeratorCreate(kCCPowerStandard).takeRetainedValue()
  CCRefrigeratorOpen(k.fridge)
  CCRefrigeratorClose(k.fridge)
}

func testTollFree0(mduct: MutableDuct) {
  let ccmduct: CCMutableDuct = mduct

  let duct: Duct = mduct
  let ccduct: CCDuct = duct
}

func testTollFree1(ccmduct: CCMutableDuct) {
  let mduct: MutableDuct = ccmduct

  let ccduct: CCDuct = ccmduct
  let duct: Duct = ccduct
}

func testChainedAliases(fridge: CCRefrigerator) {
  let _: CCRefrigeratorRef = fridge

  let _: CCFridge = fridge
  let _: CCFridgeRef = fridge
}
