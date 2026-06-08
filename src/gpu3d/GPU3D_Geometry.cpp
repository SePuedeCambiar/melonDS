#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "NDS.h"
#include "GPU.h"
#include "FIFO.h"
#include "GPU3D_Soft.h"
#include "Platform.h"
#include "GPU3D.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

template<int comp, s32 plane, bool attribs>
void ClipSegment(Vertex* outbuf, Vertex* vin, Vertex* vout)
{
    s64 factor_num = vin->Position[3] - (plane*vin->Position[comp]);
    s32 factor_den = factor_num - (vout->Position[3] - (plane*vout->Position[comp]));

#define INTERPOLATE(var)  { outbuf->var = (vin->var + ((vout->var - vin->var) * factor_num) / factor_den); }

    if (comp != 0) INTERPOLATE(Position[0]);
    if (comp != 1) INTERPOLATE(Position[1]);
    if (comp != 2) INTERPOLATE(Position[2]);
    INTERPOLATE(Position[3]);
    outbuf->Position[comp] = plane*outbuf->Position[3];

    if (attribs)
    {
        INTERPOLATE(Color[0]);
        INTERPOLATE(Color[1]);
        INTERPOLATE(Color[2]);

        INTERPOLATE(TexCoords[0]);
        INTERPOLATE(TexCoords[1]);
    }

    outbuf->Clipped = true;

#undef INTERPOLATE
}

template<int comp, bool attribs>
int ClipAgainstPlane(const GPU3D& gpu, Vertex* vertices, int nverts, int clipstart)
{
    Vertex temp[10];
    int prev, next;
    int c = clipstart;

    if (clipstart == 2)
    {
        temp[0] = vertices[0];
        temp[1] = vertices[1];
    }

    for (int i = clipstart; i < nverts; i++)
    {
        prev = i-1; if (prev < 0) prev = nverts-1;
        next = i+1; if (next >= nverts) next = 0;

        Vertex vtx = vertices[i];
        if (vtx.Position[comp] > vtx.Position[3])
        {
            if ((comp == 2) && (!(gpu.CurPolygonAttr & (1<<12)))) return 0;

            Vertex* vprev = &vertices[prev];
            if (vprev->Position[comp] <= vprev->Position[3])
            {
                ClipSegment<comp, 1, attribs>(&temp[c], &vtx, vprev);
                c++;
            }

            Vertex* vnext = &vertices[next];
            if (vnext->Position[comp] <= vnext->Position[3])
            {
                ClipSegment<comp, 1, attribs>(&temp[c], &vtx, vnext);
                c++;
            }
        }
        else
            temp[c++] = vtx;
    }

    nverts = c; c = clipstart;
    for (int i = clipstart; i < nverts; i++)
    {
        prev = i-1; if (prev < 0) prev = nverts-1;
        next = i+1; if (next >= nverts) next = 0;

        Vertex vtx = temp[i];
        if (vtx.Position[comp] < -vtx.Position[3])
        {
            Vertex* vprev = &temp[prev];
            if (vprev->Position[comp] >= -vprev->Position[3])
            {
                ClipSegment<comp, -1, attribs>(&vertices[c], &vtx, vprev);
                c++;
            }

            Vertex* vnext = &temp[next];
            if (vnext->Position[comp] >= -vnext->Position[3])
            {
                ClipSegment<comp, -1, attribs>(&vertices[c], &vtx, vnext);
                c++;
            }
        }
        else
            vertices[c++] = vtx;
    }

    // checkme
    for (int i = 0; i < c; i++)
    {
        Vertex* vtx = &vertices[i];

        vtx->Color[0] &= ~0xFFF; vtx->Color[0] += 0xFFF;
        vtx->Color[1] &= ~0xFFF; vtx->Color[1] += 0xFFF;
        vtx->Color[2] &= ~0xFFF; vtx->Color[2] += 0xFFF;
    }

    return c;
}

template<bool attribs>
int ClipPolygon(GPU3D& gpu, Vertex* vertices, int nverts, int clipstart)
{
    // clip.
    // for each vertex:
    // if it's outside, check if the previous and next vertices are inside
    // if so, place a new vertex at the edge of the view volume

    // TODO: check for 1-dot polygons
    // TODO: the hardware seems to use a different algorithm. it reacts differently to vertices with W=0
    // some vertices that should get Y=-0x1000 get Y=0x1000 for some reason on hardware. it doesn't make sense.
    // clipping seems to process the Y plane before the X plane.

    // Z clipping
    nverts = ClipAgainstPlane<2, attribs>(gpu, vertices, nverts, clipstart);

    // Y clipping
    nverts = ClipAgainstPlane<1, attribs>(gpu, vertices, nverts, clipstart);

    // X clipping
    nverts = ClipAgainstPlane<0, attribs>(gpu, vertices, nverts, clipstart);

    return nverts;
}

bool ClipCoordsEqual(Vertex* a, Vertex* b)
{
    return a->Position[0] == b->Position[0] &&
           a->Position[1] == b->Position[1] &&
           a->Position[2] == b->Position[2] &&
           a->Position[3] == b->Position[3];
}

void GPU3D::SubmitPolygon() noexcept
{
    Vertex clippedvertices[10];
    Vertex* reusedvertices[2];
    int clipstart = 0;
    int lastpolyverts = 0;

    int nverts = PolygonMode & 0x1 ? 4:3;
    int prev, next;

    // submitting a polygon starts the polygon pipeline
    // noting that for now we are only reserving one vertex slot
    // further slots only get reserved if the polygon makes it through culling/clipping
    PolygonPipeline = 8;
    VertexSlotCounter = 1;
    VertexSlotsFree = 0b11110;

    // culling
    // TODO: work out how it works on the real thing
    // the normalization part is a wild guess

    Vertex *v0, *v1, *v2, *v3;
    s64 normalX, normalY, normalZ;
    s64 dot;

    v0 = &TempVertexBuffer[0];
    v1 = &TempVertexBuffer[1];
    v2 = &TempVertexBuffer[2];
    v3 = &TempVertexBuffer[3];

    normalX = ((s64)(v0->Position[1]-v1->Position[1]) * (v2->Position[3]-v1->Position[3]))
        - ((s64)(v0->Position[3]-v1->Position[3]) * (v2->Position[1]-v1->Position[1]));
    normalY = ((s64)(v0->Position[3]-v1->Position[3]) * (v2->Position[0]-v1->Position[0]))
        - ((s64)(v0->Position[0]-v1->Position[0]) * (v2->Position[3]-v1->Position[3]));
    normalZ = ((s64)(v0->Position[0]-v1->Position[0]) * (v2->Position[1]-v1->Position[1]))
        - ((s64)(v0->Position[1]-v1->Position[1]) * (v2->Position[0]-v1->Position[0]));

    while ((((normalX>>31) ^ (normalX>>63)) != 0) ||
           (((normalY>>31) ^ (normalY>>63)) != 0) ||
           (((normalZ>>31) ^ (normalZ>>63)) != 0))
    {
        normalX >>= 4;
        normalY >>= 4;
        normalZ >>= 4;
    }

    dot = ((s64)v1->Position[0] * normalX) + ((s64)v1->Position[1] * normalY) + ((s64)v1->Position[3] * normalZ);

    bool facingview = (dot <= 0);

    if (dot < 0)
    {
        if (!(CurPolygonAttr & (1<<7)))
        {
            LastStripPolygon = NULL;
            return;
        }
    }
    else if (dot > 0)
    {
        if (!(CurPolygonAttr & (1<<6)))
        {
            LastStripPolygon = NULL;
            return;
        }
    }

    // for strips, check whether we can attach to the previous polygon
    // this requires two original vertices shared with the previous polygon, and that
    // the two polygons be of the same type

    if (PolygonMode >= 2 && LastStripPolygon)
    {
        int id0, id1;
        if (PolygonMode == 2)
        {
            if (NumConsecutivePolygons & 1)
            {
                id0 = 2;
                id1 = 1;
            }
            else
            {
                id0 = 0;
                id1 = 2;
            }

            lastpolyverts = 3;
        }
        else
        {
            id0 = 3;
            id1 = 2;

            lastpolyverts = 4;
        }

        if (LastStripPolygon->NumVertices == lastpolyverts &&
            !LastStripPolygon->Vertices[id0]->Clipped &&
            !LastStripPolygon->Vertices[id1]->Clipped)
        {
            reusedvertices[0] = LastStripPolygon->Vertices[id0];
            reusedvertices[1] = LastStripPolygon->Vertices[id1];

            clippedvertices[0] = *reusedvertices[0];
            clippedvertices[1] = *reusedvertices[1];

            clipstart = 2;
        }
    }

    for (int i = clipstart; i < nverts; i++)
        clippedvertices[i] = TempVertexBuffer[i];

    // detect lines, for the OpenGL renderer

    int polytype = 0;
    if (nverts == 3)
    {
        if (ClipCoordsEqual(&clippedvertices[0], &clippedvertices[1]) ||
            ClipCoordsEqual(&clippedvertices[0], &clippedvertices[2]) ||
            ClipCoordsEqual(&clippedvertices[1], &clippedvertices[2]))
        {
            polytype = 1;
        }
    }
    else if (nverts == 4)
    {
        // TODO
    }

    // clipping

    nverts = ClipPolygon<true>(*this, clippedvertices, nverts, clipstart);
    if (nverts == 0)
    {
        LastStripPolygon = NULL;
        return;
    }

    // reject the polygon if it's not going to fit in polygon/vertex RAM

    if (NumPolygons >= 2048 || NumVertices+nverts > 6144)
    {
        LastStripPolygon = NULL;
        DispCnt |= (1<<13);
        return;
    }

    // compute screen coordinates

    for (int i = clipstart; i < nverts; i++)
    {
        Vertex* vtx = &clippedvertices[i];

        // W is truncated to 24 bits at this point
        // if this W is zero, the polygon isn't rendered
        vtx->Position[3] &= 0x00FFFFFF;

        // viewport transform
        // note: the DS performs these divisions using a 32-bit divider
        // thus, if W is greater than 0xFFFF, some precision is sacrificed
        // to make the numbers fit into the divider
        u32 posX, posY;
        u32 w = vtx->Position[3];
        if (w == 0)
        {
            posX = 0;
            posY = 0;
        }
        else
        {
            posX = vtx->Position[0] + w;
            posY = -vtx->Position[1] + w;
            u32 den = w;

            if (w > 0xFFFF)
            {
                posX >>= 1;
                posY >>= 1;
                den  >>= 1;
            }

            den <<= 1;
            posX = ((posX * Viewport[4]) / den) + Viewport[0];
            posY = ((posY * Viewport[5]) / den) + Viewport[3];
        }

        vtx->FinalPosition[0] = posX & 0x1FF;
        vtx->FinalPosition[1] = posY & 0xFF;

        // hi-res positions
        // to consider: only do this when using the GL renderer? apply the aforementioned quirk to this?
        if (w != 0)
        {
            posX = ((((s64)(vtx->Position[0] + w) * Viewport[4]) << 4) / (((s64)w) << 1)) + (Viewport[0] << 4);
            posY = ((((s64)(-vtx->Position[1] + w) * Viewport[5]) << 4) / (((s64)w) << 1)) + (Viewport[3] << 4);

            vtx->HiresPosition[0] = posX & 0x1FFF;
            vtx->HiresPosition[1] = posY & 0xFFF;
        }
    }

    // zero-dot W check:
    // * if the polygon's vertices all have the same screen coordinates, it is considered to be zero-dot
    // * if all the vertices have a W greater than the threshold defined in register 0x04000610,
    //   the polygon is rejected, unless bit13 in the polygon attributes is set

    if (!(CurPolygonAttr & (1<<13)))
    {
        bool zerodot = true;
        bool allbehind = true;

        for (int i = 0; i < nverts; i++)
        {
            Vertex* vtx = &clippedvertices[i];

            if (vtx->FinalPosition[0] != clippedvertices[0].FinalPosition[0] ||
                vtx->FinalPosition[1] != clippedvertices[0].FinalPosition[1])
            {
                zerodot = false;
                break;
            }

            if (vtx->Position[3] <= ZeroDotWLimit)
            {
                allbehind = false;
                break;
            }
        }

        if (zerodot && allbehind)
        {
            LastStripPolygon = NULL;
            return;
        }
    }

    // build the actual polygon

    if (nverts == 4)
    {
        PolygonPipeline = 35;
        VertexSlotCounter = 1;
        if (PolygonMode & 0x2) VertexSlotsFree = 0b11100;
        else                   VertexSlotsFree = 0b11110;
    }
    else
    {
        PolygonPipeline = 26;
        VertexSlotCounter = 1;
        if (PolygonMode & 0x2) VertexSlotsFree = 0b1000;
        else                   VertexSlotsFree = 0b1110;
    }

    Polygon* poly = &CurPolygonRAM[NumPolygons++];
    poly->NumVertices = 0;

    poly->Attr = CurPolygonAttr;
    poly->TexParam = TexParam;
    poly->TexPalette = TexPalette;

    poly->Degenerate = false;
    poly->Type = 0;

    poly->FacingView = facingview;

    u32 texfmt = (TexParam >> 26) & 0x7;
    u32 polyalpha = (CurPolygonAttr >> 16) & 0x1F;
    poly->Translucent = (texfmt == 1 || texfmt == 6) || (polyalpha > 0 && polyalpha < 31);

    poly->IsShadowMask = ((CurPolygonAttr & 0x3F000030) == 0x00000030);
    poly->IsShadow = ((CurPolygonAttr & 0x30) == 0x30) && !poly->IsShadowMask;

    if (!poly->Translucent) NumOpaquePolygons++;

    poly->Type = polytype;

    if (LastStripPolygon && clipstart > 0)
    {
        if (nverts == lastpolyverts)
        {
            poly->Vertices[0] = reusedvertices[0];
            poly->Vertices[1] = reusedvertices[1];
        }
        else
        {
            Vertex v0 = *reusedvertices[0];
            Vertex v1 = *reusedvertices[1];

            CurVertexRAM[NumVertices] = v0;
            poly->Vertices[0] = &CurVertexRAM[NumVertices];
            CurVertexRAM[NumVertices+1] = v1;
            poly->Vertices[1] = &CurVertexRAM[NumVertices+1];
            NumVertices += 2;
        }

        poly->NumVertices += 2;
    }

    for (int i = clipstart; i < nverts; i++)
    {
        Vertex* vtx = &CurVertexRAM[NumVertices];
        *vtx = clippedvertices[i];
        poly->Vertices[i] = vtx;

        NumVertices++;
        poly->NumVertices++;

        vtx->FinalColor[0] = vtx->Color[0] >> 12;
        if (vtx->FinalColor[0]) vtx->FinalColor[0] = ((vtx->FinalColor[0] << 4) + 0xF);
        vtx->FinalColor[1] = vtx->Color[1] >> 12;
        if (vtx->FinalColor[1]) vtx->FinalColor[1] = ((vtx->FinalColor[1] << 4) + 0xF);
        vtx->FinalColor[2] = vtx->Color[2] >> 12;
        if (vtx->FinalColor[2]) vtx->FinalColor[2] = ((vtx->FinalColor[2] << 4) + 0xF);
    }

    // determine bounds of the polygon
    // also determine the W shift and normalize W
    // normalization works both ways
    // (ie two W's that span 12 bits or less will be brought to 16 bits)

    u32 vtop = 0, vbot = 0;
    s32 ytop = 192, ybot = 0;
    s32 xtop = 256, xbot = 0;
    u32 wsize = 0;

    for (int i = 0; i < nverts; i++)
    {
        Vertex* vtx = poly->Vertices[i];

        if (vtx->FinalPosition[1] < ytop)
        {
            xtop = vtx->FinalPosition[0];
            ytop = vtx->FinalPosition[1];
            vtop = i;
        }
        if (vtx->FinalPosition[1] > ybot || (vtx->FinalPosition[1] == ybot && vtx->FinalPosition[0] > xbot))
        {
            xbot = vtx->FinalPosition[0];
            ybot = vtx->FinalPosition[1];
            vbot = i;
        }

        u32 w = (u32)vtx->Position[3];
        if (w == 0) poly->Degenerate = true;

        while ((w >> wsize) && (wsize < 32))
            wsize += 4;
    }

    poly->VTop = vtop; poly->VBottom = vbot;
    poly->YTop = ytop; poly->YBottom = ybot;
    poly->XTop = xtop; poly->XBottom = xbot;

    if (ybot > 192) poly->Degenerate = true;

    poly->SortKey = (ybot << 8) | ytop;
    if (poly->Translucent) poly->SortKey |= 0x10000;

    poly->WBuffer = (FlushAttributes & 0x2);

    for (int i = 0; i < nverts; i++)
    {
        Vertex* vtx = poly->Vertices[i];
        s32 w, wshifted;

        // W is normalized, such that all the polygon's W values fit within 16 bits
        // the viewport transform for X/Y/Z uses the original W values, but
        // when W-buffering is used, the normalized W is used
        // W normalization is applied to separate polygons, even within strips

        if (wsize < 16)
        {
            w = vtx->Position[3] << (16 - wsize);
            wshifted = w >> (16 - wsize);
        }
        else
        {
            w = vtx->Position[3] >> (wsize - 16);
            wshifted = w << (wsize - 16);
        }

        s32 z;
        if (FlushAttributes & 0x2)
            z = wshifted;
        else if (vtx->Position[3])
            z = ((((s64)vtx->Position[2] * 0x4000) / vtx->Position[3]) + 0x3FFF) * 0x200;
        else
            z = 0x7FFE00;

        // checkme (Z<0 shouldn't be possible, but Z>0xFFFFFF is possible)
        if (z < 0) z = 0;
        else if (z > 0xFFFFFF) z = 0xFFFFFF;

        poly->FinalZ[i] = z;
        poly->FinalW[i] = w;
    }

    if (PolygonMode >= 2)
        LastStripPolygon = poly;
    else
        LastStripPolygon = NULL;
}

void GPU3D::SubmitVertex() noexcept
{
    s64 vertex[4] = {(s64)CurVertex[0], (s64)CurVertex[1], (s64)CurVertex[2], 0x1000};
    Vertex* vertextrans = &TempVertexBuffer[VertexNumInPoly];

    UpdateClipMatrix();
    vertextrans->Position[0] = (vertex[0]*ClipMatrix[0] + vertex[1]*ClipMatrix[4] + vertex[2]*ClipMatrix[8] + vertex[3]*ClipMatrix[12]) >> 12;
    vertextrans->Position[1] = (vertex[0]*ClipMatrix[1] + vertex[1]*ClipMatrix[5] + vertex[2]*ClipMatrix[9] + vertex[3]*ClipMatrix[13]) >> 12;
    vertextrans->Position[2] = (vertex[0]*ClipMatrix[2] + vertex[1]*ClipMatrix[6] + vertex[2]*ClipMatrix[10] + vertex[3]*ClipMatrix[14]) >> 12;
    vertextrans->Position[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7] + vertex[2]*ClipMatrix[11] + vertex[3]*ClipMatrix[15]) >> 12;

    // this probably shouldn't be.
    // the way color is handled during clipping needs investigation. TODO
    vertextrans->Color[0] = (VertexColor[0] << 12) + 0xFFF;
    vertextrans->Color[1] = (VertexColor[1] << 12) + 0xFFF;
    vertextrans->Color[2] = (VertexColor[2] << 12) + 0xFFF;

    if ((TexParam >> 30) == 3)
    {
        vertextrans->TexCoords[0] = ((vertex[0]*TexMatrix[0] + vertex[1]*TexMatrix[4] + vertex[2]*TexMatrix[8]) >> 24) + RawTexCoords[0];
        vertextrans->TexCoords[1] = ((vertex[0]*TexMatrix[1] + vertex[1]*TexMatrix[5] + vertex[2]*TexMatrix[9]) >> 24) + RawTexCoords[1];
    }
    else
    {
        vertextrans->TexCoords[0] = TexCoords[0];
        vertextrans->TexCoords[1] = TexCoords[1];
    }

    vertextrans->Clipped = false;

    VertexNum++;
    VertexNumInPoly++;

    switch (PolygonMode)
    {
    case 0: // triangle
        if (VertexNumInPoly == 3)
        {
            VertexNumInPoly = 0;
            SubmitPolygon();
            NumConsecutivePolygons++;
        }
        break;

    case 1: // quad
        if (VertexNumInPoly == 4)
        {
            VertexNumInPoly = 0;
            SubmitPolygon();
            NumConsecutivePolygons++;
        }
        break;

    case 2: // triangle strip
        if (NumConsecutivePolygons & 1)
        {
            Vertex tmp = TempVertexBuffer[1];
            TempVertexBuffer[1] = TempVertexBuffer[0];
            TempVertexBuffer[0] = tmp;

            VertexNumInPoly = 2;
            SubmitPolygon();
            NumConsecutivePolygons++;

            TempVertexBuffer[1] = TempVertexBuffer[2];
        }
        else if (VertexNumInPoly == 3)
        {
            VertexNumInPoly = 2;
            SubmitPolygon();
            NumConsecutivePolygons++;

            TempVertexBuffer[0] = TempVertexBuffer[1];
            TempVertexBuffer[1] = TempVertexBuffer[2];
        }
        break;

    case 3: // quad strip
        if (VertexNumInPoly == 4)
        {
            Vertex tmp = TempVertexBuffer[3];
            TempVertexBuffer[3] = TempVertexBuffer[2];
            TempVertexBuffer[2] = tmp;

            VertexNumInPoly = 2;
            SubmitPolygon();
            NumConsecutivePolygons++;

            TempVertexBuffer[0] = TempVertexBuffer[3];
            TempVertexBuffer[1] = TempVertexBuffer[2];
        }
        break;
    }

    VertexPipeline = 7;
    AddCycles(3);
}

void GPU3D::CalculateLighting() noexcept
{
    if ((TexParam >> 30) == 2)
    {
        TexCoords[0] = RawTexCoords[0] + (((s64)Normal[0]*TexMatrix[0] + (s64)Normal[1]*TexMatrix[4] + (s64)Normal[2]*TexMatrix[8]) >> 21);
        TexCoords[1] = RawTexCoords[1] + (((s64)Normal[0]*TexMatrix[1] + (s64)Normal[1]*TexMatrix[5] + (s64)Normal[2]*TexMatrix[9]) >> 21);
    }

    s32 normaltrans[3]; // should be 1 bit sign 10 bits frac
    normaltrans[0] = ((Normal[0]*VecMatrix[0] + Normal[1]*VecMatrix[4] + Normal[2]*VecMatrix[8]) << 9) >> 21;
    normaltrans[1] = ((Normal[0]*VecMatrix[1] + Normal[1]*VecMatrix[5] + Normal[2]*VecMatrix[9]) << 9) >> 21;
    normaltrans[2] = ((Normal[0]*VecMatrix[2] + Normal[1]*VecMatrix[6] + Normal[2]*VecMatrix[10]) << 9) >> 21;

    s32 c = 0;
    u32 vtxbuff[3] =
    {
        (u32)MatEmission[0] << 14,
        (u32)MatEmission[1] << 14,
        (u32)MatEmission[2] << 14
    };
    for (int i = 0; i < 4; i++)
    {
        if (!(CurPolygonAttr & (1<<i)))
            continue;

        // (credit to azusa for working out most of the details of the diff. algorithm, and essentially the entire spec. algorithm)

        // calculate dot product
        // bottom 9 bits are discarded after multiplying and before adding
        s32 dot = ((LightDirection[i][0]*normaltrans[0]) >> 9) +
                  ((LightDirection[i][1]*normaltrans[1]) >> 9) +
                  ((LightDirection[i][2]*normaltrans[2]) >> 9);

        s32 shinelevel;
        if (dot > 0)
        {
            // -- diffuse lighting --

            // convert dot to signed 11 bit int
            // then we truncate the result of the multiplications to an unsigned 20 bits before adding to the vtx color
            s32 diffdot = (dot << 21) >> 21;
            vtxbuff[0] += (MatDiffuse[0] * LightColor[i][0] * diffdot) & 0xFFFFF;
            vtxbuff[1] += (MatDiffuse[1] * LightColor[i][1] * diffdot) & 0xFFFFF;
            vtxbuff[2] += (MatDiffuse[2] * LightColor[i][2] * diffdot) & 0xFFFFF;

            // -- specular lighting --

            // reuse the dot product from diffuse lighting
            dot += normaltrans[2];

            // convert to s11, then square it, and truncate to 10 bits
            dot = (dot << 21) >> 21;
            dot = ((dot * dot) >> 10) & 0x3FF;

            // multiply dot and reciprocal, the subtract '1'
            shinelevel = ((dot * SpecRecip[i]) >> 8) - (1<<9);

            if (shinelevel < 0) shinelevel = 0;
            else
            {
                // sign extend to convert to signed 14 bit integer
                shinelevel = (shinelevel << 18) >> 18;
                if (shinelevel < 0) shinelevel = 0; // for some reason there seems to be a redundant check for <0?
                else if (shinelevel > 0x1FF) shinelevel = 0x1FF;
            }
        }
        else shinelevel = 0;

        // convert shinelevel to use for lookup in the shininess table if enabled.
        if (UseShininessTable)
        {
            shinelevel >>= 2;
            shinelevel = ShininessTable[shinelevel];
            shinelevel <<= 1;
        }

        // Note: ambient seems to be a plain bitshift
        vtxbuff[0] += ((MatSpecular[0] * shinelevel) + (MatAmbient[0] << 9)) * LightColor[i][0];
        vtxbuff[1] += ((MatSpecular[1] * shinelevel) + (MatAmbient[1] << 9)) * LightColor[i][1];
        vtxbuff[2] += ((MatSpecular[2] * shinelevel) + (MatAmbient[2] << 9)) * LightColor[i][2];

        c++;
    }

    VertexColor[0] = (vtxbuff[0] >> 14 > 31) ? 31 : (vtxbuff[0] >> 14);
    VertexColor[1] = (vtxbuff[1] >> 14 > 31) ? 31 : (vtxbuff[1] >> 14);
    VertexColor[2] = (vtxbuff[2] >> 14 > 31) ? 31 : (vtxbuff[2] >> 14);

    if (c < 1) c = 1;
    NormalPipeline = 7;
    AddCycles(c);
}


void GPU3D::BoxTest(const u32* params) noexcept
{
    Vertex cube[8];
    Vertex face[10];
    int res;

    AddCycles(254);
    GXStat &= ~(1<<1);

    s16 x0 = (s16)(params[0] & 0xFFFF);
    s16 y0 = ((s32)params[0]) >> 16;
    s16 z0 = (s16)(params[1] & 0xFFFF);
    s16 x1 = ((s32)params[1]) >> 16;
    s16 y1 = (s16)(params[2] & 0xFFFF);
    s16 z1 = ((s32)params[2]) >> 16;

    x1 += x0;
    y1 += y0;
    z1 += z0;

    cube[0].Position[0] = x0; cube[0].Position[1] = y0; cube[0].Position[2] = z0;
    cube[1].Position[0] = x1; cube[1].Position[1] = y0; cube[1].Position[2] = z0;
    cube[2].Position[0] = x1; cube[2].Position[1] = y1; cube[2].Position[2] = z0;
    cube[3].Position[0] = x0; cube[3].Position[1] = y1; cube[3].Position[2] = z0;
    cube[4].Position[0] = x0; cube[4].Position[1] = y1; cube[4].Position[2] = z1;
    cube[5].Position[0] = x0; cube[5].Position[1] = y0; cube[5].Position[2] = z1;
    cube[6].Position[0] = x1; cube[6].Position[1] = y0; cube[6].Position[2] = z1;
    cube[7].Position[0] = x1; cube[7].Position[1] = y1; cube[7].Position[2] = z1;

    UpdateClipMatrix();
    for (int i = 0; i < 8; i++)
    {
        s32 x = cube[i].Position[0];
        s32 y = cube[i].Position[1];
        s32 z = cube[i].Position[2];

        cube[i].Position[0] = ((s64)x*ClipMatrix[0] + (s64)y*ClipMatrix[4] + (s64)z*ClipMatrix[8] + (s64)0x1000*ClipMatrix[12]) >> 12;
        cube[i].Position[1] = ((s64)x*ClipMatrix[1] + (s64)y*ClipMatrix[5] + (s64)z*ClipMatrix[9] + (s64)0x1000*ClipMatrix[13]) >> 12;
        cube[i].Position[2] = ((s64)x*ClipMatrix[2] + (s64)y*ClipMatrix[6] + (s64)z*ClipMatrix[10] + (s64)0x1000*ClipMatrix[14]) >> 12;
        cube[i].Position[3] = ((s64)x*ClipMatrix[3] + (s64)y*ClipMatrix[7] + (s64)z*ClipMatrix[11] + (s64)0x1000*ClipMatrix[15]) >> 12;
    }

    // front face (-Z)
    face[0] = cube[0]; face[1] = cube[1]; face[2] = cube[2]; face[3] = cube[3];
    res = ClipPolygon<false>(*this, face, 4, 0);
    if (res > 0)
    {
        GXStat |= (1<<1);
        return;
    }

    // back face (+Z)
    face[0] = cube[4]; face[1] = cube[5]; face[2] = cube[6]; face[3] = cube[7];
    res = ClipPolygon<false>(*this, face, 4, 0);
    if (res > 0)
    {
        GXStat |= (1<<1);
        return;
    }

    // left face (-X)
    face[0] = cube[0]; face[1] = cube[3]; face[2] = cube[4]; face[3] = cube[5];
    res = ClipPolygon<false>(*this, face, 4, 0);
    if (res > 0)
    {
        GXStat |= (1<<1);
        return;
    }

    // right face (+X)
    face[0] = cube[1]; face[1] = cube[2]; face[2] = cube[7]; face[3] = cube[6];
    res = ClipPolygon<false>(*this, face, 4, 0);
    if (res > 0)
    {
        GXStat |= (1<<1);
        return;
    }

    // bottom face (-Y)
    face[0] = cube[0]; face[1] = cube[1]; face[2] = cube[6]; face[3] = cube[5];
    res = ClipPolygon<false>(*this, face, 4, 0);
    if (res > 0)
    {
        GXStat |= (1<<1);
        return;
    }

    // top face (+Y)
    face[0] = cube[2]; face[1] = cube[3]; face[2] = cube[4]; face[3] = cube[7];
    res = ClipPolygon<false>(*this, face, 4, 0);
    if (res > 0)
    {
        GXStat |= (1<<1);
        return;
    }
}

void GPU3D::PosTest() noexcept
{
    s64 vertex[4] = {(s64)CurVertex[0], (s64)CurVertex[1], (s64)CurVertex[2], 0x1000};

    UpdateClipMatrix();
    PosTestResult[0] = (vertex[0]*ClipMatrix[0] + vertex[1]*ClipMatrix[4] + vertex[2]*ClipMatrix[8] + vertex[3]*ClipMatrix[12]) >> 12;
    PosTestResult[1] = (vertex[0]*ClipMatrix[1] + vertex[1]*ClipMatrix[5] + vertex[2]*ClipMatrix[9] + vertex[3]*ClipMatrix[13]) >> 12;
    PosTestResult[2] = (vertex[0]*ClipMatrix[2] + vertex[1]*ClipMatrix[6] + vertex[2]*ClipMatrix[10] + vertex[3]*ClipMatrix[14]) >> 12;
    PosTestResult[3] = (vertex[0]*ClipMatrix[3] + vertex[1]*ClipMatrix[7] + vertex[2]*ClipMatrix[11] + vertex[3]*ClipMatrix[15]) >> 12;

    AddCycles(5);
}

void GPU3D::VecTest(u32 param) noexcept
{
    // TODO: maybe it overwrites the normal registers, too

    s16 normal[3];

    normal[0] = (s16)((param & 0x000003FF) << 6) >> 6;
    normal[1] = (s16)((param & 0x000FFC00) >> 4) >> 6;
    normal[2] = (s16)((param & 0x3FF00000) >> 14) >> 6;

    VecTestResult[0] = (normal[0]*VecMatrix[0] + normal[1]*VecMatrix[4] + normal[2]*VecMatrix[8]) >> 9;
    VecTestResult[1] = (normal[0]*VecMatrix[1] + normal[1]*VecMatrix[5] + normal[2]*VecMatrix[9]) >> 9;
    VecTestResult[2] = (normal[0]*VecMatrix[2] + normal[1]*VecMatrix[6] + normal[2]*VecMatrix[10]) >> 9;

    if (VecTestResult[0] & 0x1000) VecTestResult[0] |= 0xF000;
    if (VecTestResult[1] & 0x1000) VecTestResult[1] |= 0xF000;
    if (VecTestResult[2] & 0x1000) VecTestResult[2] |= 0xF000;

    AddCycles(4);
}




} // namespace melonDS