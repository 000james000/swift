// RUN: rm -rf %t
// RUN: mkdir -p %t

// RUN: %swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSArray -function-definitions=false -module-cache-path=%t/mcp > %t/Foundation.NSArray.printed.txt
// RUN: FileCheck -input-file %t/Foundation.NSArray.printed.txt -check-prefix=CHECK1 %s

// CHECK1: class NSMutableArray : NSArray
// CHECK1:   func setArray(otherArray: [AnyObject]!)

// RUN: %swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSKeyValueCoding -function-definitions=false -print-regular-comments -module-cache-path=%t/mcp > %t/Foundation.NSKeyValueCoding.printed.txt
// RUN: FileCheck -input-file %t/Foundation.NSKeyValueCoding.printed.txt -check-prefix=CHECK2 %s

// CHECK2: extension NSObject

// RUN: %swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSString -function-definitions=false -module-cache-path=%t/mcp > %t/Foundation.NSString.printed.txt
// RUN: FileCheck -input-file %t/Foundation.NSString.printed.txt -check-prefix=CHECK_NSSTRING %s
// RUN: FileCheck -input-file %t/Foundation.NSString.printed.txt -check-prefix=CHECK_DICTIONARY %s

// Make sure that we don't qualify 'NSErrorPointer'.
// CHECK_NSSTRING: init(contentsOfFile path: String, encoding enc: UInt, error: NSErrorPointer)

// CHECK_DICTIONARY: func propertyListFromStringsFileFormat() -> [NSObject : AnyObject]
