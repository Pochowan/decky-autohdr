# Third-party notices

decky-autohdr includes code derived from the following projects.

## AutoHDR.fx (HCRT) — MajorPainTheCactus

The SDR → HDR math in `layer/autohdr.comp` (white balance, Bezier brightness/contrast
curves, saturation, inverse Reinhard tonemap and HDR10 encoding) is a port of the ReShade
shader `AutoHDR.fx`, part of the HCRT shaders by MajorPainTheCactus.

```
MIT License

Copyright (c) 2022 MajorPainTheCactus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Xbox ATG HDR samples — Microsoft

The expanded BT.709 → BT.2020 matrix ("Vivid") and the ST.2084 (PQ) encoding come from
Microsoft's Xbox Advanced Technology Group samples
(<https://github.com/microsoft/Xbox-ATG-Samples/tree/master/Kits/ATGTK/HDR>),
via `AutoHDR.fx`.

Copyright (c) Microsoft Corporation. Licensed under the MIT License.

## Other notes

- The white-balance coefficients come from the OSL snippet referenced in `AutoHDR.fx`
  (blenderartists.org).
- The Vulkan layer is built against the Khronos
  [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0 / MIT).
- The frontend uses [@decky/ui](https://github.com/SteamDeckHomebrew/decky-frontend-lib)
  and [@decky/api](https://github.com/SteamDeckHomebrew/decky-loader) as libraries;
  see those repositories for their licenses.
