# Oculus PC SDK headers (not included)

The mod implements the LibOVR 1.10 C API, so it compiles against the Oculus PC SDK's public headers. They are not
redistributed here. To build:

1. Download the Oculus PC SDK (LibOVR) from Meta's developer site.
2. Copy its whole `LibOVR/Include/` folder - the `OVR_*.h` files AND the `Extras/` folder - into `third_party/OVR/Include/`
   here, so that `third_party/OVR/Include/OVR_CAPI.h` and `third_party/OVR/Include/Extras/OVR_CAPI_Util.h` exist.
