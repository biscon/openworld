#ifndef OW_TEMPORAL_MOTION
#define OW_TEMPORAL_MOTION
// Motion W is a tag, not a continuous validity weight.
bool temporalMotionValid(float tag) { return tag==1 || tag==2 || tag==3; }
bool temporalMotionThin(float tag) { return tag==3 || tag==-1; }
bool temporalMotionSky(float tag) { return tag==2; }
#endif
