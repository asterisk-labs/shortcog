# shortcog core

C++ GDAL driver and read paths for the shortcog profile.

Bindings live in `../bindings/`. The driver registers under the name `SHORTCOG` and activates only when the `SHORTCOG_HEADER` open option is set, so it never claims plain TIFFs.
