/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

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

// 3D engine notes
//
// vertex/polygon RAM is filled when a complete polygon is defined, after it's been culled and clipped
// 04000604 reads from bank used by renderer
// bank used by renderer is emptied at scanline ~192
// banks are swapped at scanline ~194
// TODO: needs more investigation. it's weird.
//
// clipping rules:
// * if a shared vertex in a strip is clipped, affected polygons are converted into single polygons
//   strip is resumed at the first eligible polygon
//
// clipping exhibits oddities on the real thing. bad precision? fancy algorithm? TODO: investigate.
//
// vertex color precision:
// * vertex colors are kept at 5-bit during clipping. makes for shitty results.
// * vertex colors are converted to 9-bit before drawing, as such:
//   if (x > 0) x = (x << 4) + 0xF
//   the added bias affects interpolation.
//
// depth buffer:
// Z-buffering mode: val = ((Z * 0x800 * 0x1000) / W) + 0x7FFEFF (nope, wrong. TODO update)
// W-buffering mode: val = W
//
// formula for clear depth: (GBAtek is wrong there)
// clearZ = (val * 0x200) + 0x1FF;
//
// alpha is 5-bit
//
// matrix push/pop on the position matrix are always applied to the vector matrix too, even in position-only mode
// store/restore too, probably (TODO: confirm)
// (the idea is that each position matrix has an associated vector matrix)
//
// TODO: check if translate works on the vector matrix? seems pointless
//
// viewport Y coordinates are upside-down
//
// several registers are latched upon VBlank, the renderer uses the latched registers
// latched registers include:
// DISP3DCNT
// alpha test ref value
// fog color, offset, density table
// toon table
// edge table
// clear attributes
//
// TODO: check how DISP_1DOT_DEPTH works and whether it's latched
//
// TODO: emulate GPU hanging
// * when calling BEGIN with an incomplete polygon defined
// * probably same with BOXTEST
// * when sending vertices immediately after a BOXTEST
//
// TODO: test results should probably not be presented immediately, even if we set the busy flag


// command execution notes
//
// timings given by GBAtek are for individual commands
// actual display lists have different timing characteristics
// * vertex pipeline: individual vertex commands are able to execute in parallel
//   with certain other commands
// * similarly, the normal command can execute in parallel with a subsequent vertex
// * polygon pipeline: each vertex which completes a polygon takes longer to run
//   and imposes rules on when further vertex commands can run
//   (one every 9-cycle time slot during polygon setup)
//   polygon setup time is 27 cycles for a triangle and 36 for a quad
//   except: only one time slot is taken if the polygon is rejected by culling/clipping
// * additionally, some commands (BEGIN, LIGHT_VECTOR, BOXTEST) stall the polygon pipeline


extern const u8 CmdNumParams[256];

void MatrixLoadIdentity(s32* m);

GPU3D::GPU3D(melonDS::GPU& gpu) noexcept :
    NDS(gpu.NDS),
    GPU(gpu)
{
}

void Vertex::DoSavestate(Savestate* file) noexcept
{
    file->VarArray(Position, sizeof(Position));
    file->VarArray(Color, sizeof(Color));
    file->VarArray(TexCoords, sizeof(TexCoords));

    file->Bool32(&Clipped);

    file->VarArray(FinalPosition, sizeof(FinalPosition));
    file->VarArray(FinalColor, sizeof(FinalColor));
    file->VarArray(HiresPosition, sizeof(HiresPosition));
}

void GPU3D::ResetRenderingState() noexcept
{
    RenderNumPolygons = 0;

    RenderDispCnt = 0;
    RenderAlphaRef = 0;

    memset(RenderEdgeTable, 0, 8*2);
    memset(RenderToonTable, 0, 32*2);

    RenderFogColor = 0;
    RenderFogOffset = 0;
    RenderFogShift = 0;
    memset(RenderFogDensityTable, 0, 34);

    RenderClearAttr1 = 0x3F000000;
    RenderClearAttr2 = 0x00007FFF;
}

void GPU3D::Reset() noexcept
{
    CmdFIFO.Clear();
    CmdPIPE.Clear();

    CmdStallQueue.Clear();

    ZeroDotWLimit = 0xFFFFFF;

    GXStat = 0;

    memset(ExecParams, 0, 32*4);
    ExecParamCount = 0;

    CycleCount = 0;
    VertexPipeline = 0;
    NormalPipeline = 0;
    PolygonPipeline = 0;
    VertexSlotCounter = 0;
    VertexSlotsFree = 1;

    NumPushPopCommands = 0;
    NumTestCommands = 0;

    MatrixMode = 0;

    MatrixLoadIdentity(ProjMatrix);
    MatrixLoadIdentity(PosMatrix);
    MatrixLoadIdentity(VecMatrix);
    MatrixLoadIdentity(TexMatrix);

    ClipMatrixDirty = true;
    UpdateClipMatrix();

    memset(Viewport, 0, sizeof(Viewport));

    memset(ProjMatrixStack, 0, 16*4);
    memset(PosMatrixStack, 0, 31 * 16*4);
    memset(VecMatrixStack, 0, 31 * 16*4);
    memset(TexMatrixStack, 0, 16*4);

    ProjMatrixStackPointer = 0;
    PosMatrixStackPointer = 0;
    TexMatrixStackPointer = 0;

    NumCommands = 0;
    CurCommand = 0;
    ParamCount = 0;
    TotalParams = 0;

    GeometryEnabled = false;
    RenderingEnabled = false;

    DispCnt = 0;
    AlphaRefVal = 0;
    AlphaRef = 0;

    memset(ToonTable, 0, sizeof(ToonTable));
    memset(EdgeTable, 0, sizeof(EdgeTable));

    // TODO: confirm initial polyid/color/fog values
    FogOffset = 0;
    FogColor = 0;
    memset(FogDensityTable, 0, sizeof(FogDensityTable));

    ClearAttr1 = 0x3F000000;
    ClearAttr2 = 0x00007FFF;

    ResetRenderingState();

    AbortFrame = false;

    Timestamp = 0;

    PolygonMode = 0;
    memset(CurVertex, 0, sizeof(CurVertex));
    memset(VertexColor, 0, sizeof(VertexColor));
    memset(TexCoords, 0, sizeof(TexCoords));
    memset(RawTexCoords, 0, sizeof(RawTexCoords));
    memset(Normal, 0, sizeof(Normal));

    memset(LightDirection, 0, sizeof(LightDirection));
    memset(LightColor, 0, sizeof(LightColor));
    memset(MatDiffuse, 0, sizeof(MatDiffuse));
    memset(MatAmbient, 0, sizeof(MatAmbient));
    memset(MatSpecular, 0, sizeof(MatSpecular));
    memset(MatEmission, 0, sizeof(MatSpecular));

    UseShininessTable = false;
    // Shininess table seems to be uninitialized garbage, at least on n3dsxl hw?
    // Also doesn't seem to be cleared properly unless the system is fully powered off?
    memset(ShininessTable, 0, sizeof(ShininessTable));

    PolygonAttr = 0;
    CurPolygonAttr = 0;

    TexParam = 0;
    TexPalette = 0;

    memset(PosTestResult, 0, 4*4);
    memset(VecTestResult, 0, 2*3);

    memset(TempVertexBuffer, 0, sizeof(TempVertexBuffer));
    VertexNum = 0;
    VertexNumInPoly = 0;
    NumConsecutivePolygons = 0;
    LastStripPolygon = nullptr;
    NumOpaquePolygons = 0;

    CurVertexRAM = &VertexRAM[0];
    CurPolygonRAM = &PolygonRAM[0];
    NumVertices = 0;
    NumPolygons = 0;
    CurRAMBank = 0;

    FlushRequest = 0;
    FlushAttributes = 0;

    RenderXPos = 0;
}

void GPU3D::DoSavestate(Savestate* file) noexcept
{
    file->Section("GP3D");

    CmdFIFO.DoSavestate(file);
    CmdPIPE.DoSavestate(file);

    file->Var32(&NumCommands);
    file->Var32(&CurCommand);
    file->Var32(&ParamCount);
    file->Var32(&TotalParams);

    file->Var32(&NumPushPopCommands);
    file->Var32(&NumTestCommands);

    file->Var32(&DispCnt);
    file->Var8(&AlphaRefVal);
    file->Var8(&AlphaRef);

    file->VarArray(ToonTable, 32*2);
    file->VarArray(EdgeTable, 8*2);

    file->Var32(&FogColor);
    file->Var32(&FogOffset);
    file->VarArray(FogDensityTable, 32);

    file->Var32(&ClearAttr1);
    file->Var32(&ClearAttr2);

    file->Var32(&RenderDispCnt);
    file->Var8(&RenderAlphaRef);

    file->VarArray(RenderToonTable, 32*2);
    file->VarArray(RenderEdgeTable, 8*2);

    file->Var32(&RenderFogColor);
    file->Var32(&RenderFogOffset);
    file->Var32(&RenderFogShift);
    file->VarArray(RenderFogDensityTable, 34);

    file->Var32(&RenderClearAttr1);
    file->Var32(&RenderClearAttr2);

    file->Var16(&RenderXPos);

    file->Var32(&ZeroDotWLimit);

    file->Var32(&GXStat);

    file->VarArray(ExecParams, 32*4);
    file->Var32(&ExecParamCount);
    file->Var32((u32*)&CycleCount);
    file->Var64(&Timestamp);

    file->Var32(&MatrixMode);

    file->VarArray(ProjMatrix, 16*4);
    file->VarArray(PosMatrix, 16*4);
    file->VarArray(VecMatrix, 16*4);
    file->VarArray(TexMatrix, 16*4);

    file->VarArray(ProjMatrixStack, 16*4);
    file->VarArray(PosMatrixStack, 32*16*4);
    file->VarArray(VecMatrixStack, 32*16*4);
    file->VarArray(TexMatrixStack, 16*4);

    file->Var32((u32*)&ProjMatrixStackPointer);
    file->Var32((u32*)&PosMatrixStackPointer);
    file->Var32((u32*)&TexMatrixStackPointer);

    file->VarArray(Viewport, sizeof(Viewport));

    file->VarArray(PosTestResult, 4*4);
    file->VarArray(VecTestResult, 2*3);

    file->Var32(&VertexNum);
    file->Var32(&VertexNumInPoly);
    file->Var32(&NumConsecutivePolygons);

    for (Vertex& vtx : TempVertexBuffer)
    {
        vtx.DoSavestate(file);
    }

    if (file->Saving)
    {
        u32 index = LastStripPolygon ? (u32)(LastStripPolygon - &PolygonRAM[0]) : UINT32_MAX;
        file->Var32(&index);
    }
    else
    {
        u32 index = UINT32_MAX;
        file->Var32(&index);
        LastStripPolygon = (index == UINT32_MAX) ? nullptr : &PolygonRAM[index];
    }

    file->Var32(&CurRAMBank);
    file->Var32(&NumVertices);
    file->Var32(&NumPolygons);
    file->Var32(&NumOpaquePolygons);

    file->Var32(&FlushRequest);
    file->Var32(&FlushAttributes);

    for (Vertex& vtx : VertexRAM)
    {
        vtx.DoSavestate(file);
    }

    for(int i = 0; i < 2048*2; i++)
    {
        Polygon* poly = &PolygonRAM[i];

        // this is a bit ugly, but eh
        // we can't save the pointers as-is, that's a bad idea
        if (file->Saving)
        {
            for (int j = 0; j < 10; j++)
            {
                Vertex* ptr = poly->Vertices[j];
                u32 index = ptr ? (u32)(ptr - &VertexRAM[0]) : UINT32_MAX;
                file->Var32(&index);
            }
        }
        else
        {
            for (int j = 0; j < 10; j++)
            {
                u32 index = UINT32_MAX;
                file->Var32(&index);
                poly->Vertices[j] = index == UINT32_MAX ? nullptr : &VertexRAM[index];
            }
        }

        file->Var32(&poly->NumVertices);

        file->VarArray(poly->FinalZ, sizeof(s32)*10);
        file->VarArray(poly->FinalW, sizeof(s32)*10);
        file->Bool32(&poly->WBuffer);

        file->Var32(&poly->Attr);
        file->Var32(&poly->TexParam);
        file->Var32(&poly->TexPalette);

        file->Bool32(&poly->FacingView);
        file->Bool32(&poly->Translucent);

        file->Bool32(&poly->IsShadowMask);
        file->Bool32(&poly->IsShadow);

        if (file->IsAtLeastVersion(4, 1))
            file->Var32((u32*)&poly->Type);
        else
            poly->Type = 0;

        file->Var32(&poly->VTop);
        file->Var32(&poly->VBottom);
        file->Var32((u32*)&poly->YTop);
        file->Var32((u32*)&poly->YBottom);
        file->Var32((u32*)&poly->XTop);
        file->Var32((u32*)&poly->XBottom);

        file->Var32(&poly->SortKey);

        if (!file->Saving)
        {
            poly->Degenerate = false;

            for (u32 j = 0; j < poly->NumVertices; j++)
            {
                if (poly->Vertices[j]->Position[3] == 0)
                    poly->Degenerate = true;
            }

            if (poly->YBottom > 192) poly->Degenerate = true;
        }
    }

    CmdStallQueue.DoSavestate(file);

    file->Var32((u32*)&VertexPipeline);
    file->Var32((u32*)&NormalPipeline);
    file->Var32((u32*)&PolygonPipeline);
    file->Var32((u32*)&VertexSlotCounter);
    file->Var32(&VertexSlotsFree);

    if (!file->Saving)
    {
        ClipMatrixDirty = true;
        UpdateClipMatrix();

        CurVertexRAM = &VertexRAM[CurRAMBank ? 6144 : 0];
        CurPolygonRAM = &PolygonRAM[CurRAMBank ? 2048 : 0];
    }

    file->Var32(&RenderNumPolygons);
    if (file->Saving)
    {
        for (const Polygon* p : RenderPolygonRAM)
        {
            u32 index = p ? (p - &PolygonRAM[0]) : UINT32_MAX;

            file->Var32(&index);
        }
    }
    else
    {
        for (int i = 0; i < RenderPolygonRAM.size(); ++i)
        {
            u32 index = UINT32_MAX;
            file->Var32(&index);

            RenderPolygonRAM[i] = index == UINT32_MAX ? nullptr : &PolygonRAM[index];
        }
    }

    file->VarArray(CurVertex, sizeof(s16)*3);
    file->VarArray(VertexColor, sizeof(u8)*3);
    file->VarArray(TexCoords, sizeof(s16)*2);
    file->VarArray(RawTexCoords, sizeof(s16)*2);
    file->VarArray(Normal, sizeof(s16)*3);

    file->VarArray(LightDirection, sizeof(s16)*4*3);
    file->VarArray(LightColor, sizeof(u8)*4*3);
    file->VarArray(MatDiffuse, sizeof(u8)*3);
    file->VarArray(MatAmbient, sizeof(u8)*3);
    file->VarArray(MatSpecular, sizeof(u8)*3);
    file->VarArray(MatEmission, sizeof(u8)*3);

    file->Bool32(&UseShininessTable);
    file->VarArray(ShininessTable, 128*sizeof(u8));

    file->Bool32(&AbortFrame);
    file->Bool32(&GeometryEnabled);
    file->Bool32(&RenderingEnabled);
    file->Var32(&PolygonMode);
    file->Var32(&PolygonAttr);
    file->Var32(&CurPolygonAttr);
    file->Var32(&TexParam);
    file->Var32(&TexPalette);

    RenderFrameIdentical = false;
}



void GPU3D::SetEnabled(bool geometry, bool rendering) noexcept
{
    GeometryEnabled = geometry;
    RenderingEnabled = rendering;

    if (!rendering) ResetRenderingState();
}



void MatrixLoadIdentity(s32* m);
void MatrixLoad4x4(s32* m, s32* s);
void MatrixLoad4x3(s32* m, s32* s);
void MatrixMult4x4(s32* m, s32* s);
void MatrixMult4x3(s32* m, s32* s);
void MatrixMult3x3(s32* m, s32* s);
void MatrixScale(s32* m, s32* s);
void MatrixTranslate(s32* m, s32* s);

void GPU3D::UpdateClipMatrix() noexcept
{
    if (!ClipMatrixDirty) return;
    ClipMatrixDirty = false;

    memcpy(ClipMatrix, ProjMatrix, 16*4);
    MatrixMult4x4(ClipMatrix, PosMatrix);
}



void GPU3D::AddCycles(s32 num) noexcept
{
    CycleCount += num;

    if (VertexPipeline > 0)
    {
        if (VertexPipeline > num) VertexPipeline -= num;
        else                      VertexPipeline = 0;
    }

    if (PolygonPipeline > 0)
    {
        if (PolygonPipeline > num)
        {
            PolygonPipeline -= num;
            VertexSlotCounter += num;
            while (VertexSlotCounter > 9)
            {
                VertexSlotCounter -= 9;
                VertexSlotsFree >>= 1;
            }
        }
        else
        {
            PolygonPipeline = 0;
            VertexSlotCounter = 0;
            VertexSlotsFree = 0x1;
        }
    }
}

void GPU3D::NextVertexSlot() noexcept
{
    s32 num = (9 - VertexSlotCounter) + 1;

    for (;;)
    {
        CycleCount += num;

        if (VertexPipeline > 0)
        {
            if (VertexPipeline > num) VertexPipeline -= num;
            else                      VertexPipeline = 0;
        }

        if (PolygonPipeline > 0)
        {
            if (PolygonPipeline > num)
            {
                PolygonPipeline -= num;
                VertexSlotCounter = 1;
                VertexSlotsFree >>= 1;
                if (VertexSlotsFree & 0x1)
                {
                    VertexSlotsFree &= ~0x1;
                    break;
                }
                else
                {
                    num = 9;
                    continue;
                }
            }
            else
            {
                PolygonPipeline = 0;
                VertexSlotCounter = 0;
                VertexSlotsFree = 1;
                break;
            }
        }
    }
}

void GPU3D::StallPolygonPipeline(s32 delay, s32 nonstalldelay) noexcept
{
    if (PolygonPipeline > 0)
    {
        CycleCount += PolygonPipeline + delay;

        // can be safely assumed those two will go to zero
        VertexPipeline = 0;
        NormalPipeline = 0;

        PolygonPipeline = 0;
        VertexSlotCounter = 0;
        VertexSlotsFree = 1;
    }
    else
    {
        if (VertexPipeline > nonstalldelay)
            AddCycles((VertexPipeline - nonstalldelay) + 1);
        else
            AddCycles(NormalPipeline + 1);
    }
}



s32 GPU3D::CyclesToRunFor() const noexcept
{
    if (CycleCount < 0) return 0;
    return CycleCount;
}

void GPU3D::FinishWork(s32 cycles) noexcept
{
    AddCycles(cycles);
    if (NormalPipeline)
        NormalPipeline -= std::min(NormalPipeline, cycles);

    CycleCount = 0;

    if (VertexPipeline || NormalPipeline || PolygonPipeline)
        return;

    GXStat &= ~(1<<27);
}

void GPU3D::Run() noexcept
{
    if (!GeometryEnabled || FlushRequest ||
        (CmdPIPE.IsEmpty() && !(GXStat & (1<<27))))
    {
        Timestamp = NDS.ARM9Timestamp >> NDS.ARM9ClockShift;
        return;
    }

    s32 cycles = (NDS.ARM9Timestamp >> NDS.ARM9ClockShift) - Timestamp;
    CycleCount -= cycles;
    Timestamp = NDS.ARM9Timestamp >> NDS.ARM9ClockShift;

    if (CycleCount <= 0)
    {
        while (CycleCount <= 0 && !CmdPIPE.IsEmpty())
        {
            if (NumPushPopCommands == 0) GXStat &= ~(1<<14);
            if (NumTestCommands == 0)    GXStat &= ~(1<<0);

            ExecuteCommand();
        }
    }

    if (CycleCount <= 0 && CmdPIPE.IsEmpty())
    {
        if (GXStat & (1<<27)) FinishWork(-CycleCount);
        else                  CycleCount = 0;

        if (NumPushPopCommands == 0) GXStat &= ~(1<<14);
        if (NumTestCommands == 0)    GXStat &= ~(1<<0);
    }
}


void GPU3D::CheckFIFOIRQ() noexcept
{
    bool irq = false;
    switch (GXStat >> 30)
    {
    case 1: irq = (CmdFIFO.Level() < 128); break;
    case 2: irq = CmdFIFO.IsEmpty(); break;
    }

    if (irq) NDS.SetIRQ(0, IRQ_GXFIFO);
    else     NDS.ClearIRQ(0, IRQ_GXFIFO);
}

void GPU3D::CheckFIFODMA() noexcept
{
    if (CmdFIFO.Level() < 128)
        NDS.CheckDMAs(0, 0x07);
}


bool YSort(Polygon* a, Polygon* b)
{
    // polygon sorting rules:
    // * opaque polygons come first
    // * polygons with lower bottom Y come first
    // * upon equal bottom Y, polygons with lower top Y come first
    // * upon equal bottom AND top Y, original ordering is used
    // the SortKey is calculated as to implement these rules

    return a->SortKey < b->SortKey;
}

void GPU3D::VBlank() noexcept
{
    if (GeometryEnabled)
    {
        if (RenderingEnabled)
        {
            if (FlushRequest)
            {
                if (NumPolygons)
                {
                    // separate translucent polygons from opaque ones

                    u32 io = 0, it = NumOpaquePolygons;
                    for (u32 i = 0; i < NumPolygons; i++)
                    {
                        Polygon* poly = &CurPolygonRAM[i];
                        if (poly->Translucent)
                            RenderPolygonRAM[it++] = poly;
                        else
                            RenderPolygonRAM[io++] = poly;
                    }

                    // apply Y-sorting

                    std::stable_sort(RenderPolygonRAM.begin(),
                        RenderPolygonRAM.begin() + ((FlushAttributes & 0x1) ? NumOpaquePolygons : NumPolygons),
                        YSort);
                }

                RenderNumPolygons = NumPolygons;
                RenderFrameIdentical = false;
            }
            else
            {
                RenderFrameIdentical = RenderDispCnt == DispCnt
                    && RenderAlphaRef == AlphaRef
                    && RenderClearAttr1 == ClearAttr1
                    && RenderClearAttr2 == ClearAttr2
                    && RenderFogColor == FogColor
                    && RenderFogOffset == FogOffset * 0x200
                    && memcmp(RenderEdgeTable, EdgeTable, 8*2) == 0
                    && memcmp(RenderFogDensityTable + 1, FogDensityTable, 32) == 0
                    && memcmp(RenderToonTable, ToonTable, 32*2) == 0;
            }

            RenderDispCnt = DispCnt;
            RenderAlphaRef = AlphaRef;

            memcpy(RenderEdgeTable, EdgeTable, 8*2);
            memcpy(RenderToonTable, ToonTable, 32*2);

            RenderFogColor = FogColor;
            RenderFogOffset = FogOffset * 0x200;
            RenderFogShift = (RenderDispCnt >> 8) & 0xF;
            RenderFogDensityTable[0] = FogDensityTable[0];
            memcpy(&RenderFogDensityTable[1], FogDensityTable, 32);
            RenderFogDensityTable[33] = FogDensityTable[31];

            RenderClearAttr1 = ClearAttr1;
            RenderClearAttr2 = ClearAttr2;
        }

        if (FlushRequest)
        {
            CurRAMBank = CurRAMBank?0:1;
            CurVertexRAM = &VertexRAM[CurRAMBank ? 6144 : 0];
            CurPolygonRAM = &PolygonRAM[CurRAMBank ? 2048 : 0];

            NumVertices = 0;
            NumPolygons = 0;
            NumOpaquePolygons = 0;

            FlushRequest = 0;
        }
    }
}


void GPU3D::SetRenderXPos(u16 xpos, u16 mask) noexcept
{
    if (!RenderingEnabled) return;

    RenderXPos = (RenderXPos & ~mask) | (xpos & mask & 0x01FF);
}

}