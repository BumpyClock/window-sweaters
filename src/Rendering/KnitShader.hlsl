// SPDX-License-Identifier: GPL-3.0-only
// Ring lighting, fray and shadow adapted from src/Knitting/knit_math.c.
cbuffer Parameters : register(b0) {
    float2 ringSize;
    float outerRadius;
    float inset;
    float innerRadius;
    float2 stripOrigin;
    float scale;
    float relief;
    float band;
    float pitch;
    float phase;
    uint side;
    float inverseScale;
};
cbuffer Placement : register(b1) {
    float4 hole;
    float2 offset;
    float holeRadius;
    uint hasHole;
};
Texture2D<float4> colour : register(t0);
Texture2D<float4> heights : register(t1);
Texture1D<float2> xCoordinates : register(t2);
Texture1D<float2> yCoordinates : register(t3);

float4 Vertex(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}

float Box(float2 p, float2 origin, float2 size, float radius, out float2 gradient) {
    float2 centre = p - origin - size * .5;
    float2 sign = float2(centre.x < 0 ? -1 : 1, centre.y < 0 ? -1 : 1);
    float2 q = abs(centre) - size * .5 + radius;
    if (q.x > 0 && q.y > 0) {
        float len = sqrt(q.x * q.x + q.y * q.y);
        gradient = sign * q / len;
        return len - radius;
    }
    if (q.x > q.y) {
        gradient = float2(sign.x, 0);
        return q.x - radius;
    }
    gradient = float2(0, sign.y);
    return q.y - radius;
}

float Height(int2 xy, float centre) {
    float4 h = heights.Load(int3(xy, 0));
    return h.a > 0 ? h.b / h.a : centre;
}

uint Mix(uint h) {
    h ^= h >> 16; h *= 0x85ebca6b;
    h ^= h >> 13; h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

float4 Mask(float4 pixel, int2 xy) {
    float2 unused;
    float coverage = hasHole ? saturate(Box(xy + .5, hole.xy, hole.zw - hole.xy, holeRadius, unused) + .5) : 1;
    return floor(pixel * coverage + .5) / 255;
}

float4 Copy(float4 position : SV_Position) : SV_Target {
    int2 xy = int2(position.xy - offset);
    return Mask(floor(colour.Load(int3(xy, 0)) * 255 + .5), xy);
}

float4 Knit(float4 position : SV_Position) : SV_Target {
    int2 xy = int2(position.xy - offset);
    float4 pixel = floor(colour.Load(int3(xy, 0)) * 255 + .5);
    float2 xCoordinate = xCoordinates.Load(int2(xy.x, 0));
    float2 yCoordinate = yCoordinates.Load(int2(xy.y, 0));
    float2 p = float2(xCoordinate.x, yCoordinate.x);
    float2 gradient, unused;
    float outside = -Box(p, 0, ringSize, outerRadius, gradient);
    if (pixel.a > 0) {
        bool hole = inset > 0 && all(ringSize > 2 * inset);
        float inside = hole ? Box(p, inset, ringSize - 2 * inset, innerRadius, unused)
            : min(ringSize.x, ringSize.y) * .5 - outside;
        float outDistance = max(0, outside);
        inside = max(0, inside);
        float u = 2 * outDistance / max(.0001, outDistance + inside) - 1;
        float bend = (u < 0 ? -1 : 1) * pow(abs(u), 1.6);
        float s = sin(bend * 1.05), c = cos(bend * 1.05);
        float slope = 0;
        float4 centre = heights.Load(int3(xy, 0));
        if (centre.a > 0) {
            float h0 = centre.b / centre.a;
            float k2 = relief * scale * .6;
            slope += .26 * (Height(xy + int2(1, 0), h0) - Height(xy - int2(1, 0), h0)) * k2 / .87;
            slope += .42 * (Height(xy + int2(0, 1), h0) - Height(xy - int2(0, 1), h0)) * k2 / .87;
        }
        float ndl = max(0, (gradient.x * s * .26 + gradient.y * s * .42 + c * .87) / .87 + slope);
        float e2 = u * u;
        float shade = (.56 + .44 * ndl) * (1 - .22 * e2 * e2);
        pixel.rgb = floor(min(pixel.a, pixel.rgb * shade + .5));
        float depth = band * .07;
        if (depth * scale >= .25 && outside < depth + inverseScale) {
            float along = side == 0 ? p.x : side == 1 ? p.y :
                side == 2 ? ringSize.x - p.x : ringSize.y - p.y;
            float bump = .5 - .5 * cos(6.28318530718 * (along - phase) / pitch);
            int2 cell = int2(xCoordinate.y, yCoordinate.y);
            uint grain = Mix(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u);
            float fibre = ((grain & 255u) / 255.0 - .5) * .6;
            float erode = depth * saturate(.85 * (1 - bump) + fibre);
            float coverage = saturate((outside - erode) * scale + 1);
            pixel = floor(pixel * coverage + .5);
        }
    }
    if (pixel.a < 255) {
        float4 edge = float4(p.y, ringSize.x - p.x, ringSize.y - p.y, p.x);
        uint owner = 0;
        for (uint i = 1; i < 4; ++i) if (edge[i] < edge[owner]) owner = i;
        if (owner == side && -outside >= -.5 * band) {
            float distance = Box(p - float2(.07, .12) * band, 0, ringSize, outerRadius, unused);
            float t = saturate((distance + .1 * band) / (.1 * band + .36 * band));
            float shadow = 1 - t * t * (3 - 2 * t);
            pixel.a += floor(.3 * shadow * shadow * (255 - pixel.a) + .5);
        }
    }
    return Mask(pixel, xy);
}
