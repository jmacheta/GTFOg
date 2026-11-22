# mcumgr maps CBOR responses onto its model classes by reflection (Jackson).
-keep class io.runtime.mcumgr.** { *; }
-keepattributes Signature, InnerClasses, EnclosingMethod, *Annotation*
-keep class com.fasterxml.jackson.annotation.** { *; }
