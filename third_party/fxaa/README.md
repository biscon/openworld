NVIDIA FXAA SDK 11, March 21 2011 archive; FxaaShader.h with one Vulkan portability change: HLSL FxaaTexOff adds the texel offset to UV instead of using a dynamic SampleLevel offset, avoiding shaderImageGatherExtended. Algorithm/preset unchanged.
https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA.zip
https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf
Engine wrapper selects preset 3, HLSL 4, UNORM encoded input/output.
See LICENSE.txt and the copyright notice in FxaaShader.h.

This software contains source code provided by NVIDIA Corporation.

SDK archive SHA-256: a584af1b311c81d9e4a1959709972bf6eec8e101f5667bb50071cb0210617a76
