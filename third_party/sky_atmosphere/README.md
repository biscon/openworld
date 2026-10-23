# Atmosphere algorithm attribution

The atmosphere shaders adapt the LUT decomposition, isotropic multiple-scattering
closure and segment integration described by Sébastien Hillaire in *A Scalable
and Production Ready Sky and Atmosphere Rendering Technique* (EGSR 2020).

Reference: https://github.com/sebh/UnrealEngineSkyAtmosphere

The reference's Epic Games copyright and MIT license are retained in `LICENSE`.
Engine resource management, Vulkan integration, tests and CPU reference integration
are local implementations. The D3D11 application and its dependencies are not vendored.
The transmittance coordinate mapping follows the Bruneton/Hillaire atmosphere
parameterization described in the referenced work.
