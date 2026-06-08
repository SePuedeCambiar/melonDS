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

void MatrixLoadIdentity(s32* m);
void MatrixLoad4x4(s32* m, s32* s);
void MatrixLoad4x3(s32* m, s32* s);
void MatrixMult4x4(s32* m, s32* s);
void MatrixMult4x3(s32* m, s32* s);
void MatrixMult3x3(s32* m, s32* s);
void MatrixScale(s32* m, s32* s);
void MatrixTranslate(s32* m, s32* s);
extern const u8 CmdNumParams[256] =
{
    // 0x00
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x10
    1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3,
    0, 0, 0,
    // 0x20
    1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0,
    // 0x30
    1, 1, 1, 1, 32,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x40
    1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x50
    1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x60
    1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x70
    3, 2, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x80+
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void GPU3D::CmdFIFOWrite(const CmdFIFOEntry& entry) noexcept
{
    if (CmdFIFO.IsEmpty() && !CmdPIPE.IsFull())
    {
        CmdPIPE.Write(entry);
    }
    else
    {
        if (CmdFIFO.IsFull())
        {
            // store it to the stall queue. stall the system.
            // worst case is if a STMxx opcode causes this, which is why our stall queue
            // has 64 entries. this is less complicated than trying to make STMxx stall-able.

            CmdStallQueue.Write(entry);
            NDS.GXFIFOStall();
            return;
        }

        CmdFIFO.Write(entry);
    }

    GXStat |= (1<<27);

    if (entry.Command == 0x11 || entry.Command == 0x12)
    {
        GXStat |= (1<<14); // push/pop matrix
        NumPushPopCommands++;
    }
    else if (entry.Command == 0x70 || entry.Command == 0x71 || entry.Command == 0x72)
    {
        GXStat |= (1<<0); // box/pos/vec test
        NumTestCommands++;
    }
}

GPU3D::CmdFIFOEntry GPU3D::CmdFIFORead() noexcept
{
    CmdFIFOEntry ret = CmdPIPE.Read();

    if (CmdPIPE.Level() <= 2)
    {
        if (!CmdFIFO.IsEmpty())
            CmdPIPE.Write(CmdFIFO.Read());
        if (!CmdFIFO.IsEmpty())
            CmdPIPE.Write(CmdFIFO.Read());

        // empty stall queue if needed
        // CmdFIFO should not be full at this point.
        if (!CmdStallQueue.IsEmpty())
        {
            while (!CmdStallQueue.IsEmpty())
            {
                if (CmdFIFO.IsFull()) break;
                CmdFIFOEntry entry = CmdStallQueue.Read();
                CmdFIFOWrite(entry);
            }

            if (CmdStallQueue.IsEmpty())
                NDS.GXFIFOUnstall();
        }

        CheckFIFODMA();
        CheckFIFOIRQ();
    }

    return ret;
}

void GPU3D::ExecuteCommand() noexcept
{
    CmdFIFOEntry entry = CmdFIFORead();

    //printf("FIFO: processing %02X %08X. Levels: FIFO=%d, PIPE=%d\n", entry.Command, entry.Param, CmdFIFO->Level(), CmdPIPE->Level());

    // each FIFO entry takes 1 cycle to be processed
    // commands (presumably) run when all the needed parameters have been read
    // which is where we add the remaining cycles if any

    u32 paramsRequiredCount = CmdNumParams[entry.Command];
    if (paramsRequiredCount <= 1)
    {
        // fast path for command which only have a single parameter

        /*printf("[GXS:%08X] 0x%02X,  0x%08X", GXStat, entry.Command, entry.Param);*/

        switch (entry.Command)
        {
        case 0x10: // matrix mode
            VertexPipelineCmdDelayed4();
            MatrixMode = entry.Param & 0x3;
            break;

        case 0x11: // push matrix
            VertexPipelineCmdDelayed4();
            NumPushPopCommands--;
            if (MatrixMode == 0)
            {
                if (ProjMatrixStackPointer > 0) GXStat |= (1<<15);

                memcpy(ProjMatrixStack, ProjMatrix, 16*4);
                ProjMatrixStackPointer++;
                ProjMatrixStackPointer &= 0x1;
            }
            else if (MatrixMode == 3)
            {
                if (TexMatrixStackPointer > 0) GXStat |= (1<<15);

                memcpy(TexMatrixStack, TexMatrix, 16*4);
                TexMatrixStackPointer++;
                TexMatrixStackPointer &= 0x1;
            }
            else
            {
                if (PosMatrixStackPointer > 30) GXStat |= (1<<15);

                memcpy(PosMatrixStack[PosMatrixStackPointer & 0x1F], PosMatrix, 16*4);
                memcpy(VecMatrixStack[PosMatrixStackPointer & 0x1F], VecMatrix, 16*4);
                PosMatrixStackPointer++;
                PosMatrixStackPointer &= 0x3F;
            }
            AddCycles(16);
            break;

        case 0x12: // pop matrix
            VertexPipelineCmdDelayed4();
            NumPushPopCommands--;
            if (MatrixMode == 0)
            {
                if (ProjMatrixStackPointer == 0) GXStat |= (1<<15);

                ProjMatrixStackPointer--;
                ProjMatrixStackPointer &= 0x1;
                memcpy(ProjMatrix, ProjMatrixStack, 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            else if (MatrixMode == 3)
            {
                if (TexMatrixStackPointer == 0) GXStat |= (1<<15);

                TexMatrixStackPointer--;
                TexMatrixStackPointer &= 0x1;
                memcpy(TexMatrix, TexMatrixStack, 16*4);
                AddCycles(17);
            }
            else
            {
                s32 offset = (s32)(entry.Param << 26) >> 26;
                PosMatrixStackPointer -= offset;
                PosMatrixStackPointer &= 0x3F;

                if (PosMatrixStackPointer > 30) GXStat |= (1<<15);

                memcpy(PosMatrix, PosMatrixStack[PosMatrixStackPointer & 0x1F], 16*4);
                memcpy(VecMatrix, VecMatrixStack[PosMatrixStackPointer & 0x1F], 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            break;

        case 0x13: // store matrix
            VertexPipelineCmdDelayed4();
            if (MatrixMode == 0)
            {
                memcpy(ProjMatrixStack, ProjMatrix, 16*4);
            }
            else if (MatrixMode == 3)
            {
                memcpy(TexMatrixStack, TexMatrix, 16*4);
            }
            else
            {
                u32 addr = entry.Param & 0x1F;
                if (addr > 30) GXStat |= (1<<15);

                memcpy(PosMatrixStack[addr], PosMatrix, 16*4);
                memcpy(VecMatrixStack[addr], VecMatrix, 16*4);
            }
            AddCycles(16);
            break;

        case 0x14: // restore matrix
            VertexPipelineCmdDelayed4();
            if (MatrixMode == 0)
            {
                memcpy(ProjMatrix, ProjMatrixStack, 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            else if (MatrixMode == 3)
            {
                memcpy(TexMatrix, TexMatrixStack, 16*4);
                AddCycles(17);
            }
            else
            {
                u32 addr = entry.Param & 0x1F;
                if (addr > 30) GXStat |= (1<<15);

                memcpy(PosMatrix, PosMatrixStack[addr], 16*4);
                memcpy(VecMatrix, VecMatrixStack[addr], 16*4);
                ClipMatrixDirty = true;
                AddCycles(35);
            }
            break;

        case 0x15: // identity
            VertexPipelineCmdDelayed4();
            if (MatrixMode == 0)
            {
                MatrixLoadIdentity(ProjMatrix);
                ClipMatrixDirty = true;
                AddCycles(18);
            }
            else if (MatrixMode == 3)
                MatrixLoadIdentity(TexMatrix);
            else
            {
                MatrixLoadIdentity(PosMatrix);
                if (MatrixMode == 2)
                    MatrixLoadIdentity(VecMatrix);
                ClipMatrixDirty = true;
                AddCycles(18);
            }
            break;

        case 0x20: // vertex color
            VertexPipelineCmdDelayed6();
            {
                u32 c = entry.Param;
                u32 r = c & 0x1F;
                u32 g = (c >> 5) & 0x1F;
                u32 b = (c >> 10) & 0x1F;
                VertexColor[0] = r;
                VertexColor[1] = g;
                VertexColor[2] = b;
            }
            break;

        case 0x21: // normal
            VertexPipelineCmdDelayed4();
            Normal[0] = (s16)((entry.Param & 0x000003FF) << 6) >> 6;
            Normal[1] = (s16)((entry.Param & 0x000FFC00) >> 4) >> 6;
            Normal[2] = (s16)((entry.Param & 0x3FF00000) >> 14) >> 6;
            CalculateLighting();
            break;

        case 0x22: // texcoord
            VertexPipelineCmdDelayed4();
            RawTexCoords[0] = entry.Param & 0xFFFF;
            RawTexCoords[1] = entry.Param >> 16;
            if ((TexParam >> 30) == 1)
            {
                TexCoords[0] = (RawTexCoords[0]*TexMatrix[0] + RawTexCoords[1]*TexMatrix[4] + TexMatrix[8] + TexMatrix[12]) >> 12;
                TexCoords[1] = (RawTexCoords[0]*TexMatrix[1] + RawTexCoords[1]*TexMatrix[5] + TexMatrix[9] + TexMatrix[13]) >> 12;
            }
            else
            {
                TexCoords[0] = RawTexCoords[0];
                TexCoords[1] = RawTexCoords[1];
            }
            break;

        case 0x24: // 10-bit vertex
            VertexPipelineSubmitCmd();
            CurVertex[0] = (entry.Param & 0x000003FF) << 6;
            CurVertex[1] = (entry.Param & 0x000FFC00) >> 4;
            CurVertex[2] = (entry.Param & 0x3FF00000) >> 14;
            SubmitVertex();
            break;

        case 0x25: // vertex XY
            VertexPipelineSubmitCmd();
            CurVertex[0] = entry.Param & 0xFFFF;
            CurVertex[1] = entry.Param >> 16;
            SubmitVertex();
            break;

        case 0x26: // vertex XZ
            VertexPipelineSubmitCmd();
            CurVertex[0] = entry.Param & 0xFFFF;
            CurVertex[2] = entry.Param >> 16;
            SubmitVertex();
            break;

        case 0x27: // vertex YZ
            VertexPipelineSubmitCmd();
            CurVertex[1] = entry.Param & 0xFFFF;
            CurVertex[2] = entry.Param >> 16;
            SubmitVertex();
            break;

        case 0x28: // 10-bit delta vertex
            VertexPipelineSubmitCmd();
            CurVertex[0] += (s16)((entry.Param & 0x000003FF) << 6) >> 6;
            CurVertex[1] += (s16)((entry.Param & 0x000FFC00) >> 4) >> 6;
            CurVertex[2] += (s16)((entry.Param & 0x3FF00000) >> 14) >> 6;
            SubmitVertex();
            break;

        case 0x29: // polygon attributes
            VertexPipelineCmdDelayed8();
            PolygonAttr = entry.Param;
            break;

        case 0x2A: // texture param
            VertexPipelineCmdDelayed8();
            TexParam = entry.Param;
            break;

        case 0x2B: // texture palette
            VertexPipelineCmdDelayed8();
            TexPalette = entry.Param & 0x1FFF;
            break;

        case 0x30: // diffuse/ambient material
            VertexPipelineCmdDelayed6();
            MatDiffuse[0] = entry.Param & 0x1F;
            MatDiffuse[1] = (entry.Param >> 5) & 0x1F;
            MatDiffuse[2] = (entry.Param >> 10) & 0x1F;
            MatAmbient[0] = (entry.Param >> 16) & 0x1F;
            MatAmbient[1] = (entry.Param >> 21) & 0x1F;
            MatAmbient[2] = (entry.Param >> 26) & 0x1F;
            if (entry.Param & 0x8000)
            {
                VertexColor[0] = MatDiffuse[0];
                VertexColor[1] = MatDiffuse[1];
                VertexColor[2] = MatDiffuse[2];
            }
            AddCycles(3);
            break;

        case 0x31: // specular/emission material
            VertexPipelineCmdDelayed6();
            MatSpecular[0] = entry.Param & 0x1F;
            MatSpecular[1] = (entry.Param >> 5) & 0x1F;
            MatSpecular[2] = (entry.Param >> 10) & 0x1F;
            MatEmission[0] = (entry.Param >> 16) & 0x1F;
            MatEmission[1] = (entry.Param >> 21) & 0x1F;
            MatEmission[2] = (entry.Param >> 26) & 0x1F;
            UseShininessTable = (entry.Param & 0x8000) != 0;
            AddCycles(3);
            break;

        case 0x32: // light direction
            StallPolygonPipeline(8 + 1,  2); // 0x32 can run 6 cycles after a vertex
            {
                u32 l = entry.Param >> 30;
                s16 dir[3];
                dir[0] = (s16)((entry.Param & 0x000003FF) << 6) >> 6;
                dir[1] = (s16)((entry.Param & 0x000FFC00) >> 4) >> 6;
                dir[2] = (s16)((entry.Param & 0x3FF00000) >> 14) >> 6;
                // the order of operations here is very specific: discard bottom 12 bits -> negate -> then sign extend to convert to 11 bit signed int
                // except for when used to calculate the specular reciprocal; then it's: sign extend -> discard lsb -> negate.
                LightDirection[l][0] = (-((dir[0]*VecMatrix[0] + dir[1]*VecMatrix[4] + dir[2]*VecMatrix[8] ) >> 12) << 21) >> 21;
                LightDirection[l][1] = (-((dir[0]*VecMatrix[1] + dir[1]*VecMatrix[5] + dir[2]*VecMatrix[9] ) >> 12) << 21) >> 21;
                LightDirection[l][2] = (-((dir[0]*VecMatrix[2] + dir[1]*VecMatrix[6] + dir[2]*VecMatrix[10]) >> 12) << 21) >> 21;
                s32 den =              -(((dir[0]*VecMatrix[2] + dir[1]*VecMatrix[6] + dir[2]*VecMatrix[10]) << 9) >> 21) + (1<<9);

                if (den == 0) SpecRecip[l] = 0;
                else SpecRecip[l] = (1<<18) / den;
            }
            AddCycles(5);
            break;

        case 0x33: // light color
            VertexPipelineCmdDelayed8();
            {
                u32 l = entry.Param >> 30;
                LightColor[l][0] = entry.Param & 0x1F;
                LightColor[l][1] = (entry.Param >> 5) & 0x1F;
                LightColor[l][2] = (entry.Param >> 10) & 0x1F;
            }
            AddCycles(1);
            break;

        case 0x40: // begin polygons
            StallPolygonPipeline(1, 0);
            // TODO: check if there was a polygon being defined but incomplete
            // such cases seem to freeze the GPU
            PolygonMode = entry.Param & 0x3;
            VertexNum = 0;
            VertexNumInPoly = 0;
            NumConsecutivePolygons = 0;
            LastStripPolygon = NULL;
            CurPolygonAttr = PolygonAttr;
            break;

        case 0x41: // end polygons
            VertexPipelineCmdDelayed8();
            // TODO: research this?
            // it doesn't seem to have any effect whatsoever, but
            // its timing characteristics are different from those of other
            // no-op commands
            break;

        case 0x50: // flush
            VertexPipelineCmdDelayed4();
            FlushRequest = 1;
            FlushAttributes = entry.Param & 0x3;
            CycleCount = 325;
            // probably safe to just reset all pipelines
            // but needs checked
            VertexPipeline = 0;
            NormalPipeline = 0;
            PolygonPipeline = 0;
            VertexSlotCounter = 0;
            VertexSlotsFree = 1;
            break;

        case 0x60: // viewport x1,y1,x2,y2
            VertexPipelineCmdDelayed8();
            // note: viewport Y coordinates are upside-down
            Viewport[0] = entry.Param & 0xFF;                             // x0
            Viewport[1] = (191 - ((entry.Param >> 8) & 0xFF)) & 0xFF;     // y0
            Viewport[2] = (entry.Param >> 16) & 0xFF;                     // x1
            Viewport[3] = (191 - (entry.Param >> 24)) & 0xFF;             // y1
            Viewport[4] = (Viewport[2] - Viewport[0] + 1) & 0x1FF;          // width
            Viewport[5] = (Viewport[1] - Viewport[3] + 1) & 0xFF;           // height
            break;

        case 0x72: // vec test
            VertexPipelineCmdDelayed6();
            NumTestCommands--;
            VecTest(entry.Param);
            break;

        default:
            VertexPipelineCmdDelayed4();
            //printf("!! UNKNOWN GX COMMAND %02X %08X\n", entry.Command, entry.Param);
            break;
        }
    }
    else
    {
        ExecParams[ExecParamCount] = entry.Param;
        ExecParamCount++;

        if (ExecParamCount == 1)
        {
            // delay the first command entry as needed
            switch (entry.Command)
            {
            // commands that stall the polygon pipeline
            case 0x23: VertexPipelineSubmitCmd(); break;
            case 0x34:
            case 0x71:
                VertexPipelineCmdDelayed8();
                break;
            case 0x70: StallPolygonPipeline(10 + 1, 0); break;
            default: VertexPipelineCmdDelayed4(); break;
            }
        }
        else
        {
            AddCycles(1);

            if (ExecParamCount >= paramsRequiredCount)
            {
                /*printf("[GXS:%08X] 0x%02X,  ", GXStat, entry.Command);
                for (int k = 0; k < ExecParamCount; k++) printf("0x%08X, ", ExecParams[k]);
                printf("\n");*/

                ExecParamCount = 0;

                switch (entry.Command)
                {
                case 0x16: // load 4x4
                    if (MatrixMode == 0)
                    {
                        MatrixLoad4x4(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixLoad4x4(TexMatrix, (s32*)ExecParams);
                        AddCycles(10);
                    }
                    else
                    {
                        MatrixLoad4x4(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                            MatrixLoad4x4(VecMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    break;

                case 0x17: // load 4x3
                    if (MatrixMode == 0)
                    {
                        MatrixLoad4x3(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixLoad4x3(TexMatrix, (s32*)ExecParams);
                        AddCycles(7);
                    }
                    else
                    {
                        MatrixLoad4x3(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                            MatrixLoad4x3(VecMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(18);
                    }
                    break;

                case 0x18: // mult 4x4
                    if (MatrixMode == 0)
                    {
                        MatrixMult4x4(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 16);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixMult4x4(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 16);
                    }
                    else
                    {
                        MatrixMult4x4(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixMult4x4(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 16);
                        }
                        else AddCycles(35 - 16);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x19: // mult 4x3
                    if (MatrixMode == 0)
                    {
                        MatrixMult4x3(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 12);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixMult4x3(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 12);
                    }
                    else
                    {
                        MatrixMult4x3(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixMult4x3(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 12);
                        }
                        else AddCycles(35 - 12);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x1A: // mult 3x3
                    if (MatrixMode == 0)
                    {
                        MatrixMult3x3(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 9);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixMult3x3(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 9);
                    }
                    else
                    {
                        MatrixMult3x3(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixMult3x3(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 9);
                        }
                        else AddCycles(35 - 9);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x1B: // scale
                    if (MatrixMode == 0)
                    {
                        MatrixScale(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 3);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixScale(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 3);
                    }
                    else
                    {
                        MatrixScale(PosMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 3);
                    }
                    break;

                case 0x1C: // translate
                    if (MatrixMode == 0)
                    {
                        MatrixTranslate(ProjMatrix, (s32*)ExecParams);
                        ClipMatrixDirty = true;
                        AddCycles(35 - 3);
                    }
                    else if (MatrixMode == 3)
                    {
                        MatrixTranslate(TexMatrix, (s32*)ExecParams);
                        AddCycles(33 - 3);
                    }
                    else
                    {
                        MatrixTranslate(PosMatrix, (s32*)ExecParams);
                        if (MatrixMode == 2)
                        {
                            MatrixTranslate(VecMatrix, (s32*)ExecParams);
                            AddCycles(35 + 30 - 3);
                        }
                        else AddCycles(35 - 3);
                        ClipMatrixDirty = true;
                    }
                    break;

                case 0x23: // full vertex
                    CurVertex[0] = ExecParams[0] & 0xFFFF;
                    CurVertex[1] = ExecParams[0] >> 16;
                    CurVertex[2] = ExecParams[1] & 0xFFFF;
                    SubmitVertex();
                    break;

                case 0x34: // shininess table
                    {
                        for (int i = 0; i < 128; i += 4)
                        {
                            u32 val = ExecParams[i >> 2];
                            ShininessTable[i + 0] = val & 0xFF;
                            ShininessTable[i + 1] = (val >> 8) & 0xFF;
                            ShininessTable[i + 2] = (val >> 16) & 0xFF;
                            ShininessTable[i + 3] = val >> 24;
                        }
                    }
                    break;

                case 0x71: // pos test
                    NumTestCommands -= 2;
                    CurVertex[0] = ExecParams[0] & 0xFFFF;
                    CurVertex[1] = ExecParams[0] >> 16;
                    CurVertex[2] = ExecParams[1] & 0xFFFF;
                    PosTest();
                    break;

                case 0x70: // box test
                    NumTestCommands -= 3;
                    BoxTest(ExecParams);
                    break;

                default:
                    __builtin_unreachable();
                }
            }
        }
    }
}


} // namespace melonDS