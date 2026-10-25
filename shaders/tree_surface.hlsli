float heightAt(uint offset, uint width, uint2 p, uint eroded) {
    float2 h = heights[offset + p.y * width + p.x];
    return eroded != 0 ? h.y : h.x;
}
float3 vertex(Patch p, uint2 local) {
    local = terrainStitchedVertex(local, p.geometry.w, p.topology.x);
    uint2 grid = p.geometry.xy + local * p.geometry.z;
    float h = heightAt(p.heights.x, p.heights.z, grid >> p.topology.z, p.topology.w);
    if (terrainStationaryCoarse(local, p.geometry.w, p.topology.x, p.topology.y)) {
        uint2 a, b, c;
        float3 w;
        uint level = p.topology.z + 1;
        terrainParentTriangle(grid, 1u << level, (p.heights.z - 1) << p.topology.z, a, b, c, w);
        h = terrainTrianglePoint(heightAt(p.heights.y, p.heights.w, a >> level, p.topology.w).xxx,
                                 heightAt(p.heights.y, p.heights.w, b >> level, p.topology.w).xxx,
                                 heightAt(p.heights.y, p.heights.w, c >> level, p.topology.w).xxx, w)
                .x;
    }
    float2 xz = p.rectangle.xy + float2(grid) * p.rectangle.z;
    return float3(xz.x, h, xz.y);
}
bool sampleTriangle(float2 queryPosition, float3 a, float3 b, float3 c, out float height, out float2 slope) {
    float2 e = b.xz - a.xz, f = c.xz - a.xz, q = queryPosition - a.xz;
    float det = e.x * f.y - e.y * f.x;
    height = 0;
    slope = 0;
    if (abs(det) < 1e-10)
        return false;
    float u = (q.x * f.y - q.y * f.x) / det, v = (e.x * q.y - e.y * q.x) / det;
    if (u < -.00001 || v < -.00001 || u + v > 1.00001)
        return false;
    height = a.y + u * (b.y - a.y) + v * (c.y - a.y);
    float3 n = cross(b - a, c - a);
    slope = -n.xz / n.y;
    return true;
}
bool treeSurface(Job job, float2 queryPosition, out float height, out float2 slope) {
    height = 0;
    slope = 0;
    for (uint i = 0; i < job.config.w; ++i) {
        Patch p = job.patches[i];
        float2 q = ((queryPosition - p.rectangle.xy) / p.rectangle.z - float2(p.geometry.xy)) / p.geometry.z;
        if (any(q < -.0001) || any(q > float(p.geometry.w) + .0001))
            continue;
        int2 base = clamp(int2(floor(q)), 0, int(p.geometry.w) - 1);
        // A stitched edge sampleTriangle can span its adjacent collapsed quad.
        for (int z = max(0, base.y - 1); z <= min(int(p.geometry.w) - 1, base.y + 1); ++z)
            for (int x = max(0, base.x - 1); x <= min(int(p.geometry.w) - 1, base.x + 1); ++x) {
                float3 a = vertex(p, uint2(x, z)), b = vertex(p, uint2(x + 1, z));
                float3 c = vertex(p, uint2(x, z + 1)), d = vertex(p, uint2(x + 1, z + 1));
                if (sampleTriangle(queryPosition, a, c, d, height, slope) ||
                    sampleTriangle(queryPosition, a, d, b, height, slope))
                    return true;
            }
    }
    return false;
}
