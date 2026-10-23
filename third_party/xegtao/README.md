# XeGTAO attribution

The horizon integral, depth-weighting, edge estimation and geometric-normal formulas
in `shaders/gtao.hlsli` are adapted from Intel XeGTAO's `XeGTAO.hlsli` and `XeGTAO.h`:
https://github.com/GameTechDev/XeGTAO (archived master revision `a5b1686`, accessed 2026-10-22).
Copyright (C) 2016-2021, Intel Corporation; MIT license reproduced here.
Authors: Filip Strugar and Steve Mccalla.

This is an engine adaptation, not a vendored copy of the upstream implementation.
It uses Vulkan bindings, explicit odd-extent handling, FP32 view depth, nearest-surface
half-resolution representatives, fixed spatial noise, bounded searches, conservative
screen boundaries, a spatial edge-path filter and native-depth guided reconstruction.
No upstream sample/framework is built. The GTAO algorithm originates in Jimenez et al.,
Practical Real-Time Strategies for Accurate Indirect Occlusion (2016).
