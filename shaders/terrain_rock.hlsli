#ifndef OW_TERRAIN_ROCK
#define OW_TERRAIN_ROCK
// Evaluate slope with evaluateBaseTerrain(local,true), using continuous domains.
// Material colour and plant suitability must classify the same geographic field.
float terrainRockWeight(float slope) {return smoothstep(.47,1.0,slope);}
float grassRockSuitability(float rock) {return 1-smoothstep(.15,.50,rock);}
#endif
