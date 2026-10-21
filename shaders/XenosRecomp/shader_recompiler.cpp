#include "shader_recompiler.h"

#include <regex>
#include <cstdlib>
#include <cstdio>
#include "shader_common.h"
#include <stdexcept>

static constexpr char SWIZZLES[] = 
{ 
    'x',
    'y', 
    'z', 
    'w', 
    '0', 
    '1',
    '_',
    '_'
};

static constexpr const char* USAGE_TYPES[] =
{
    "float4", // POSITION
    "float4", // BLENDWEIGHT
    "uint4", // BLENDINDICES
    "float4", // NORMAL: the game stores them as 16-bit integers or as float
    "float4", // PSIZE
    "float4", // TEXCOORD
    "float4", // TANGENT
    "float4", // BINORMAL
    "float4", // TESSFACTOR
    "float4", // POSITIONT
    "float4", // COLOR
    "float4", // FOG
    "float4", // DEPTH
    "float4", // SAMPLE
};

static constexpr const char* USAGE_VARIABLES[] =
{
    "Position",
    "BlendWeight",
    "BlendIndices",
    "Normal",
    "PointSize",
    "TexCoord",
    "Tangent",
    "Binormal",
    "TessFactor",
    "PositionT",
    "Color",
    "Fog",
    "Depth",
    "Sample"
};

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "NORMAL",
    "PSIZE",
    "TEXCOORD",
    "TANGENT",
    "BINORMAL",
    "TESSFACTOR",
    "POSITIONT",
    "COLOR",
    "FOG",
    "DEPTH",
    "SAMPLE"
};

struct DeclUsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

// Interpolator locations (the renderer links VS outputs and PS inputs by these).
static constexpr DeclUsageLocation USAGE_LOCATIONS[] =
{
    { DeclUsage::Position, 0, 0 },
    { DeclUsage::Normal, 0, 1 },
    { DeclUsage::Tangent, 0, 2 },
    { DeclUsage::Binormal, 0, 3 },
    { DeclUsage::TexCoord, 0, 4 },
    { DeclUsage::TexCoord, 1, 5 },
    { DeclUsage::TexCoord, 2, 6 },
    { DeclUsage::TexCoord, 3, 7 },
    { DeclUsage::Color, 0, 8 },
    { DeclUsage::BlendIndices, 0, 9 },
    { DeclUsage::BlendWeight, 0, 10 },
    { DeclUsage::Color, 1, 11 },
    { DeclUsage::TexCoord, 4, 12 },
    { DeclUsage::TexCoord, 5, 13 },
    { DeclUsage::TexCoord, 6, 14 },
    { DeclUsage::TexCoord, 7, 15 },
    { DeclUsage::Position, 1, 15 },
};

static constexpr std::pair<DeclUsage, size_t> INTERPOLATORS[] =
{
    { DeclUsage::TexCoord, 0 },
    { DeclUsage::TexCoord, 1 },
    { DeclUsage::TexCoord, 2 },
    { DeclUsage::TexCoord, 3 },
    { DeclUsage::TexCoord, 4 },
    { DeclUsage::TexCoord, 5 },
    { DeclUsage::TexCoord, 6 },
    { DeclUsage::TexCoord, 7 },
    { DeclUsage::TexCoord, 8 },
    { DeclUsage::TexCoord, 9 },
    { DeclUsage::TexCoord, 10 },
    { DeclUsage::TexCoord, 11 },
    { DeclUsage::TexCoord, 12 },
    { DeclUsage::TexCoord, 13 },
    { DeclUsage::TexCoord, 14 },
    { DeclUsage::TexCoord, 15 },
    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 }
    // COLOR2..5 are declared and consumed too.
    , { DeclUsage::Color, 2 }
    , { DeclUsage::Color, 3 }
    , { DeclUsage::Color, 4 }
    , { DeclUsage::Color, 5 }
};

static constexpr std::string_view TEXTURE_DIMENSIONS[] = 
{
    "2D",
    "3D", 
    "Cube" 
};

static FetchDestinationSwizzle getDestSwizzle(uint32_t dstSwizzle, uint32_t index)
{
    return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
}

void ShaderRecompiler::printDstSwizzle(uint32_t dstSwizzle, bool operand)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
            out += SWIZZLES[operand ? uint32_t(swizzle) : i];
    }
}

void ShaderRecompiler::printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle == FetchDestinationSwizzle::Zero)
        {
            indent();
            println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
        }
        else if (swizzle == FetchDestinationSwizzle::One)
        {
            indent();
            println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
        }
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
    if (instr.isPredicated)
        abrirPredicado(instr.predicateCondition);
    else
        cerrarPredicado();
    const size_t marcaPredicado = out.size();

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    auto findResult = vertexElements.find(address);
    if (findResult == vertexElements.end())
        throw std::runtime_error("FETCH de vertices sin elemento declarado");

    /*
     * A fetch at an index the shader computes. The Xenos vfetch reads its vertex at the index in its source register;
     * only r0.x (the vertex's own index) is what a vertex attribute gives. Dead Space 2's particles keep one record per
     * particle and read it at vertex / 4 (58 of its 161 vertex shaders fetch like this); as attributes, every vertex
     * read its own record, past the particles' and with the wrong corners. The shader reads these itself (dsVfetch,
     * from the vertex buffer bound whole as a storage buffer); the renderer describes each one at runtime (D3D patches
     * format, stride and offset into the fetch per draw), at the element's position in the declaration.
     */
    if (instr.srcRegister != 0 || instr.srcSwizzle != 0)
    {
        usaLecturaManual = true;
        const uint32_t k = elementOrdinal.at(address);
        print("remapInput(dsVfetch({}, r{}.{}, {}), g_UboLecturas.v[{}].w).", k, uint32_t(instr.srcRegister),
            SWIZZLES[instr.srcSwizzle & 0x3], instr.isIndexRounded ? "true" : "false", k);
        printDstSwizzle(instr.dstSwizzle, true);
        out += ";\n";
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        cerrarSiEscribePredicado(marcaPredicado);
        return;
    }

    // D3D patches the fetch swizzle according to the vertex declaration; the
    // native renderer passes the mapping through g_InputRemap.
    int32_t remapLocation = -1;
    for (auto& usageLocation : USAGE_LOCATIONS)
    {
        if (usageLocation.usage == findResult->second.usage && usageLocation.usageIndex == findResult->second.usageIndex)
        {
            remapLocation = int32_t(usageLocation.location);
            break;
        }
    }
    if (remapLocation >= 0)
        out += "remapInput(float4(";

    switch (findResult->second.usage)
    {
    // The game declares normals, tangents and binormals as float4 (USAGE_TYPES): the
    // native renderer supplies them already normalized through the Vulkan format.

    case DeclUsage::TexCoord:
        print("tfetchTexcoord(g_SwappedTexcoords, ");
        break;
    }

    print("i{}{}", USAGE_VARIABLES[uint32_t(findResult->second.usage)], uint32_t(findResult->second.usageIndex));

    switch (findResult->second.usage)
    {

    case DeclUsage::TexCoord:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }

    if (remapLocation >= 0)
        print("), g_InputRemap({}))", remapLocation);

    out += '.';
    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    cerrarSiEscribePredicado(marcaPredicado);
}

void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, bool bicubic)
{
    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)
        return;

    if (instr.isPredicated)
        abrirPredicado(instr.predCondition);
    else
        cerrarPredicado();
    const size_t marcaPredicado = out.size();

    auto printSrcRegister = [&](size_t componentCount)
        {
            print("r{}.", instr.srcRegister);

            for (size_t i = 0; i < componentCount; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
        };

    std::string constName;
    const char* constNamePtr = nullptr;

    auto findResult = samplers.find(instr.constIndex);
    if (findResult != samplers.end())
    {
        constNamePtr = findResult->second;

    }
    else if (auto vertical = samplers.find(instr.constIndex - 16);
             !isPixelShader && instr.constIndex >= 16 && vertical != samplers.end())
    {
        // Vertex textures: the XDK puts sampler sN of a vertex shader at fetch constant 16 + N (Dead Space 2's
        // modulate_sampler_vs, s1, is fetched as constant 17).
        constNamePtr = vertical->second;
    }
    else
    {
        constName = fmt::format("s{}", instr.constIndex);
        constNamePtr = constName.c_str();
    }
    dimensionesBuscadas[constNamePtr] |= 1u << uint32_t(instr.dimension);


    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";
    switch (instr.opcode)
    {
    case FetchOpcode::TextureFetch:
    {

        out += "tfetch";
        break;
    }
    case FetchOpcode::GetTextureWeights:
    {
        out += "getWeights";
        break;
    }
    }

    std::string_view dimension;
    uint32_t componentCount = 0;

    switch (instr.dimension)
    {
    case TextureDimension::Texture1D:
        dimension = "1D";
        componentCount = 1;
        break;
    case TextureDimension::Texture2D:
        dimension = "2D";
        componentCount = 2;
        break;
    case TextureDimension::Texture3D:
        dimension = "3D";
        componentCount = 3;
        break;
    case TextureDimension::TextureCube:
        dimension = "Cube";
        componentCount = 3;
        break;
    }

    out += dimension;

    // A vertex shader has no derivatives, so its 2D fetches sample mip 0 (tfetch2DVertice, written into the
    // shader at the end of recompile()).
    if (!isPixelShader && instr.opcode == FetchOpcode::TextureFetch && instr.dimension == TextureDimension::Texture2D)
    {
        out += "Vertice";
        usaTfetchVertice = true;
    }


    print("({0}_Texture{1}DescriptorIndex, {0}_SamplerDescriptorIndex, ", constNamePtr, dimension);
    printSrcRegister(componentCount);

    switch (instr.dimension)
    {
    case TextureDimension::Texture2D:
        // The last argument is 1/size of the slot, so the texture does not have to be queried.
        print(", float2({}, {}), {}_InvTamano", instr.offsetX * 0.5f, instr.offsetY * 0.5f, constNamePtr);
        break;
    case TextureDimension::TextureCube:
        out += ", cubeMapData";
        break;
    }

    out += ").";

    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    cerrarSiEscribePredicado(marcaPredicado);
}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
    if (instr.isPredicated)
        abrirPredicado(instr.predicateCondition);
    else
        cerrarPredicado();
    const size_t marcaPredicado = out.size();

    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    auto op = [&](size_t operand)
        {
            size_t reg = 0;
            size_t swizzle = 0;
            bool select = true;
            bool negate = false;
            bool abs = false;

            switch (operand)
            {
            case SCALAR_CONSTANT_0:
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = false;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            case SCALAR_CONSTANT_1:
                reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                swizzle = instr.src3Swizzle;
                select = true;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            default:
                switch (operand)
                {
                case VECTOR_0:
                    reg = instr.src1Register;
                    swizzle = instr.src1Swizzle;
                    select = instr.src1Select;
                    negate = instr.src1Negate;
                    break;
                case VECTOR_1:
                    reg = instr.src2Register;
                    swizzle = instr.src2Swizzle;
                    select = instr.src2Select;
                    negate = instr.src2Negate;
                    break;
                case VECTOR_2:
                case SCALAR_0:
                case SCALAR_1:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = instr.src3Select;
                    negate = instr.src3Negate;
                    break;
                }

                if (select)
                {
                    abs = (reg & 0x80) != 0;
                    reg &= 0x3F;
                }
                else
                {
                    abs = instr.absConstants;
                }

                break;
            }

            std::string regFormatted;

            if (select)
            {
                regFormatted = fmt::format("r{}", reg);
            }
            else
            {
                // The bits correspond to the first constant and the following ones,
                // not to the whole instruction. Temporary operands do not count.
                const bool relativa = operand == VECTOR_0 ? instr.const0Relative :
                    operand == VECTOR_1 ? (instr.src1Select ? instr.const0Relative : instr.const1Relative) :
                    (instr.src1Select && instr.src2Select ? instr.const0Relative : instr.const1Relative);
                auto findResult = float4Constants.find(reg);
                if (findResult != float4Constants.end())
                {
                    const char* constantName = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                    if (findResult->second->registerCount > 1)
                    {
                        {
                            regFormatted = fmt::format("{}({}{})", constantName,
                                reg - findResult->second->registerIndex, relativa ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                        }
                    }
                    else
                    {
                        if (relativa)
                            throw std::runtime_error("direccionamiento relativo de constante escalar pendiente");
                        regFormatted = constantName;
                    }
                }
                else
                {
                    if (relativa)
                        throw std::runtime_error("direccionamiento relativo sin constante declarada pendiente");
                    regFormatted = fmt::format("c{}", reg);
                }
            }

            std::string result;

            if (negate)
                result += '-';

            if (abs)
                result += "abs(";

            result += regFormatted;
            result += '.';

            switch (operand)
            {
            case VECTOR_0:
            case VECTOR_1:
            case VECTOR_2:
            {
                uint32_t mask;

                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Dp2Add:
                    mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                    break;

                case AluVectorOpcode::Dp3:
                    mask = 0b111;
                    break;

                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Max4:
                    mask = 0b1111;
                    break;

                default:
                    mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                    break;
                }

                for (size_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 0x1)
                        result += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                }

                break;
            }

            case SCALAR_0:
            case SCALAR_CONSTANT_0:
                result += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                break;

            case SCALAR_1:
            case SCALAR_CONSTANT_1:
                result += SWIZZLES[swizzle & 0x3];
                break;
            }

            if (abs)
                result += ")";

            return result;
        };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::KillEq:
        indent();
        println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGt:
        indent();
        println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGe:
        indent();
        println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillNe:
        indent();
        println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    }

    bool closeIfBracket = false;

    std::string_view exportRegister;
    if (instr.exportData)
    {
        if (isPixelShader)
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
                exportRegister = "oC0";
                break;        
            case ExportRegister::PSColor1:
                exportRegister = "oC1";
                break;        
            case ExportRegister::PSColor2:
                exportRegister = "oC2";
                break;            
            case ExportRegister::PSColor3:
                exportRegister = "oC3";
                break;           
            case ExportRegister::PSDepth:
                exportRegister = "oDepth";
                break;
            }
        }
        else
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::VSPosition:
                exportRegister = "oPos";


                break;

            default:
            {
                auto findResult = interpolators.find(instr.vectorDest);
                if (findResult == interpolators.end())
                    throw std::runtime_error("exportacion de vertices sin interpolador declarado");
                exportRegister = findResult->second;
                break;
            }
            }
        }
    }

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        indent();
        print("p0 = {} == 0.0 && {} ", op(VECTOR_0), op(VECTOR_1));

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::SetpEqPush:
            out += "==";
            break;
        case AluVectorOpcode::SetpNePush:
            out += "!=";
            break;
        case AluVectorOpcode::SetpGtPush:
            out += ">";
            break;
        case AluVectorOpcode::SetpGePush:
            out += ">=";
            break;
        }

        out += " 0.0;\n";
    }
    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
        println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0));
    }

    uint32_t vectorWriteMask = instr.vectorWriteMask;
    if (instr.exportData)
        vectorWriteMask &= ~instr.scalarWriteMask;

    if (vectorWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.vectorDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ";

        if (instr.vectorSaturate)
            out += "saturate(";

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Add:
            print("{} + {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Mul:
            print("{} * {}", op(VECTOR_0), op(VECTOR_1));
            break;

        /*
         * The Xenos MOV is a max with itself.
         *
         * The Xenos has no copy instruction: the assembler writes it as MAX dst, src, src
         * (and MAXs for the scalar channel). The translator emitted it as is, so the library
         * had 452 `max(a, a)` in the pixel shaders and 482 in the vertex shaders: almost a
         * thousand operations that are a copy. max(a, a) == a exactly (also with NaN and with
         * both zeros, which is the only thing that sets IEEE min/max apart), so the operand is
         * emitted directly.
         *
         * The `a0 = ...` of MaxA/MaxAs/MaxAsf is not touched: it is handled separately and
         * stays the same.
         */
        case AluVectorOpcode::Max:
        case AluVectorOpcode::MaxA:
        case AluVectorOpcode::Min:
        {
            std::string a = op(VECTOR_0);
            std::string b = op(VECTOR_1);
            if (a == b)
                out += a;
            else
                print("{}({}, {})", instr.vectorOpcode == AluVectorOpcode::Min ? "min" : "max", a, b);
            break;
        }

        case AluVectorOpcode::Seq:
            print("{} == {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sgt:
            print("{} > {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sge:
            print("{} >= {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sne:
            print("{} != {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Frc:
            print("frac({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Trunc:
            print("trunc({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Floor:
            print("floor({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Mad:
            print("{} * {} + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndEq:
            print("select({} == 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGe:
            print("select({} >= 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGt:
            print("select({} > 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Dp3:
            print("dot({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dp2Add:
            print("dot({}, {}) + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Cube:
            print("cube(r{}, cubeMapData)", instr.src1Register);
            break;

        case AluVectorOpcode::Max4:
            print("max4({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
            print("p0 ? 0.0 : {} + 1.0", op(VECTOR_0));
            break;

        case AluVectorOpcode::KillEq:
            print("any({} == {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGt:
            print("any({} > {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGe:
            print("any({} >= {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillNe:
            print("any({} != {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dst:
            print("dst({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;
        }

        if (instr.vectorSaturate)
            out += ')';

        out += ";\n";
    }

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            indent();
            out += "p0 = ";

            switch (instr.scalarOpcode)
            {
            case AluScalarOpcode::SetpEq:
                print("{} == 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpNe:
                print("{} != 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGt:
                print("{} > 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGe:
                print("{} >= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpInv:
                print("{} == 1.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpPop:
                print("{} - 1.0 <= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpClr:
                out += "false";
                break;

            case AluScalarOpcode::SetpRstr:
                print("{} == 0.0", op(SCALAR_0));
                break;
            }

            out += ";\n";
        }

        indent();
        out += "ps = ";
        if (instr.scalarSaturate)
            out += "saturate(";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::Adds:
            print("{} + {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::AddsPrev:
            print("{} + ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Muls:
            print("{} * {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::MulsPrev:
        case AluScalarOpcode::MulsPrev2:
            print("{} * ps", op(SCALAR_0));
            break;

        // Same as above, for the scalar channel: MAXs dst, src, src is the Xenos MOV.
        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
        case AluScalarOpcode::Mins:
        {
            std::string a = op(SCALAR_0);
            std::string b = op(SCALAR_1);
            if (a == b)
                out += a;
            else
                print("{}({}, {})", instr.scalarOpcode == AluScalarOpcode::Mins ? "min" : "max", a, b);
            break;
        }

        case AluScalarOpcode::Seqs:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sgts:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sges:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Snes:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Frcs:
            print("frac({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Truncs:
            print("trunc({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Floors:
            print("floor({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Exp:
            print("exp2({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
            print("clamp(log2({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcpf:
        case AluScalarOpcode::Rcp:
            print("clamp(rcp({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsqf:
        case AluScalarOpcode::Rsq:
            print("clamp(rsqrt({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Subs:
            print("{} - {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::SubsPrev:
            print("{} - ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out += "p0 ? 0.0 : 1.0";
            break;

        case AluScalarOpcode::SetpInv:
            // Xenos: with src == 1 the counter goes back to 0 (Xenia's ucode.h, kSetpInv). Without that
            // case the counter stayed at 1 and later p0 blocks did not run.
            print("p0 ? 0.0 : ({0} == 0.0 ? 1.0 : {0})", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpPop:
            print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpClr:
            out += "FLT_MAX";
            break;

        case AluScalarOpcode::SetpRstr:
            print("p0 ? 0.0 : {}", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsEq:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGt:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGe:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsNe:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsOne:
            print("{} == 1.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sqrt:
            print("sqrt({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            print("{} * {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            print("{} + {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            print("{} - {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Sin:
            print("sin({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Cos:
            print("cos({})", op(SCALAR_0));
            break;
        }

        if (instr.scalarSaturate)
            out += ')';

        out += ";\n";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::MaxAs:
            indent();
            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0));
            break;     
        case AluScalarOpcode::MaxAsf:
            indent();
            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0));
            break;
        }
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;

    if (scalarWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.scalarDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((scalarWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ps;\n";
    }

    if (instr.exportData)
    {
        uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;

        for (size_t i = 0; i < 4; i++)
        {
            uint32_t mask = 1 << i;
            if (zeroMask & mask)
            {
                indent();
                println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
            }
            else if (oneMask & mask)
            {
                indent();
                println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
            }
        }
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        indent();
        out += "clip(ps != 0.0 ? -1 : 1);\n";
    }

    if (closeIfBracket)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    cerrarSiEscribePredicado(marcaPredicado);
}

void ShaderRecompiler::printDeko3dConstants(const ConstantTableContainer* constantTableContainer)
{
    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? 224 : 256) - constantInfo->registerIndex;
                println("#define {}(INDEX) select((INDEX) < {}, g_Ubo{}.v[{} + min(INDEX, {})], 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} g_Ubo{}.v[{}]", constantName, shaderName, constantInfo->registerIndex.get());
            }
            break;
        }

        case RegisterSet::Sampler:
        {
            // One texture per register with the type the constant table gives it, at binding R; its sampler at
            // binding R too (another set: GLSL's combined sampler takes the texture's binding, SDK glsl.cpp).
            // deko3d has 32 texture bindings per stage: 0-15 for the registers and 16-31 for the 2D pairs.
            const uint32_t reg = constantInfo->registerIndex;
            const auto typeInfo = reinterpret_cast<const TypeInfo*>(constantTableData + constantInfo->typeInfo);
            const char* dimension = "2D";
            switch (typeInfo->parameterType)
            {
            case ParameterType::Texture3D:
            case ParameterType::Sampler3D:
                dimension = "3D";
                break;
            case ParameterType::TextureCube:
            case ParameterType::SamplerCube:
                dimension = "Cube";
                break;
            default:
                break;
            }

            if (reg >= 16)
                println("#error \"{}: sampler register {} has no deko3d binding (0-15)\"", constantName, reg);

            println("[[vk::binding({}, 0)]] Texture{}<float4> {}_Textura;", reg, dimension, constantName);
            println("[[vk::binding({}, 1)]] SamplerState {}_Muestreo;", reg, constantName);
            println("#define {0}_Texture{1}DescriptorIndex {0}_Textura", constantName, dimension);
            println("#define {0}_SamplerDescriptorIndex {0}_Muestreo", constantName);
            if (strcmp(dimension, "2D") == 0)
            {
                // tfetch2DSombraMin's pair arrives in the 3D index word of the same register.
                println("[[vk::binding({}, 0)]] Texture2D<float4> {}_Pareja;", 16 + reg, constantName);
                println("#define {0}_Texture3DDescriptorIndex {0}_Pareja", constantName);
            }

            const uint32_t invBase = 360 + reg * 8;
            println("#define {}_InvTamano float2(DS_COMPARTIDA_FLOAT({}), DS_COMPARTIDA_FLOAT({}))",
                constantName, invBase, invBase + 4);
            break;
        }

        default:
            break;
        }
    }
}

// dsVfetch: the Xenos vfetch done by the shader (see recompile(VertexFetchInstruction)). Descriptor k (uniform buffer 3):
// x = storage buffer | format << 8 | signed << 16 | integer << 17, y = offset, z = stride (bytes), w = g_InputRemap
// code. The buffers hold the guest's words in host order (the renderer swaps them as vertex buffers): each format's
// first component is in the low bits of its word, as the vertex attributes read them.
static const char* const kLecturaManualHlsl = R"(struct DsBloqueLecturas { uint4 v[16]; };
[[vk::binding(3, 4)]] ConstantBuffer<DsBloqueLecturas> g_UboLecturas;
[[vk::binding(0, 5)]] ByteAddressBuffer g_VerticesB0;
[[vk::binding(1, 5)]] ByteAddressBuffer g_VerticesB1;
[[vk::binding(2, 5)]] ByteAddressBuffer g_VerticesB2;
[[vk::binding(3, 5)]] ByteAddressBuffer g_VerticesB3;

// Kept small on purpose: it is inlined at every fetch, and the first version (a switch per format) made 70 KB of DKSH
// per shader that uam took 4-9 s to compile on the console. One read of 4 words, then one layout for every format.
float4 dsVfetch(uint k, float indiceReal, bool redondeado)
{
    uint4 d = g_UboLecturas.v[k];
    uint direccion = uint(max(redondeado ? round(indiceReal) : floor(indiceReal), 0.0)) * d.z + d.y;
    uint b = d.x & 0xFF;
    uint formato = (d.x >> 8) & 0x3F;
    uint4 w;
    [branch] if (b == 0) w = g_VerticesB0.Load4(direccion);
    else [branch] if (b == 1) w = g_VerticesB1.Load4(direccion);
    else [branch] if (b == 2) w = g_VerticesB2.Load4(direccion);
    else w = g_VerticesB3.Load4(direccion);
    // 32-bit floats (k_32_FLOAT 36, k_32_32 37, k_32_32_32 57, k_32_32_32_32 38) and halves (31, 32).
    uint n = formato == 36 ? 1u : formato == 37 ? 2u : formato == 57 ? 3u : 4u;
    if (formato == 36 || formato == 37 || formato == 57 || formato == 38)
        return float4(asfloat(w.x), n > 1 ? asfloat(w.y) : 0.0, n > 2 ? asfloat(w.z) : 0.0, n > 3 ? asfloat(w.w) : 1.0);
    if (formato == 31 || formato == 32)
        return float4(f16tof32(w.x), f16tof32(w.x >> 16), formato == 32 ? f16tof32(w.y) : 0.0,
                      formato == 32 ? f16tof32(w.y >> 16) : 1.0);
    // Packed integers: each component's bits from a word at a shift (the first component in the low bits).
    uint4 v;
    uint4 bits;
    if (formato == 6)        { v = w.xxxx >> uint4(0, 8, 16, 24); bits = uint4(8, 8, 8, 8); n = 4; }
    else if (formato == 7)   { v = w.xxxx >> uint4(0, 10, 20, 30); bits = uint4(10, 10, 10, 2); n = 4; }
    else if (formato == 25)  { v = w.xxxx >> uint4(0, 16, 0, 0); bits = uint4(16, 16, 16, 16); n = 2; }
    else if (formato == 26)  { v = w.xxyy >> uint4(0, 16, 0, 16); bits = uint4(16, 16, 16, 16); n = 4; }
    else                     { v = w; bits = uint4(32, 32, 32, 32); n = formato == 33 ? 1u : formato == 34 ? 2u : 4u; }
    uint4 mascara = select(bits == 32, 0xFFFFFFFFu, (1u << bits) - 1u);
    v &= mascara;
    float4 r;
    if ((d.x >> 16) & 1)
    {
        int4 s = int4(v << (32 - bits)) >> int4(32 - bits);
        r = ((d.x >> 17) & 1) ? float4(s) : max(float4(s) / float4((1u << (bits - 1)) - 1u), -1.0);
    }
    else
    {
        r = ((d.x >> 17) & 1) ? float4(v) : float4(v) / float4(mascara);
    }
    return float4(r.x, n > 1 ? r.y : 0.0, n > 2 ? r.z : 0.0, n > 3 ? r.w : 1.0);
}

)";

void ShaderRecompiler::recompile(const uint8_t* shaderData, const std::string_view& include)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);

    /*
     * Dead Space 2's containers are format 2008 (0x102A1100 / 0x102A1101): only the two high bytes are
     * compared.
     */
    assert((shaderContainer->flags & 0xFFFF0000) == 0x102A0000);
    assert(shaderContainer->constantTableOffset != NULL);

    out += include;
    out += '\n';
    // Where tfetch2DVertice is written if this vertex shader samples a texture (end of this function).
    const size_t marcaTrasInclude = out.size();

    isPixelShader = (shaderContainer->flags & 0x1) == 0;

    const auto constantTableContainer = reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset);
    constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

        if (constantInfo->registerSet == RegisterSet::Float4)
        {
            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);
        }
        else if (constantInfo->registerSet == RegisterSet::Sampler)
        {
            samplers.emplace(constantInfo->registerIndex, constantName);
        }
    }

    printDeko3dConstants(constantTableContainer);

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Bool)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);
            println("\t#define {} (1 << {})", constantName, constantInfo->registerIndex + (isPixelShader ? 16 : 0));
            // The CTAB numbers b0.. and CF uses 128.. for pixels.
            // The g_Booleans packing keeps its 16 bits per stage.
            boolConstants.emplace(constantInfo->registerIndex + (isPixelShader ? 128 : 0), constantName);
        }
    }

    out += '\n';

    const auto shader = reinterpret_cast<const Shader*>(shaderData + shaderContainer->shaderOffset);

    out += "void main(\n";

    if (isPixelShader)
    {
        out += "\tin float4 iPos : SV_Position,\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\tin float4 i{0}{1} : {2}{1},", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);

        out += "\tin bool iFace : SV_IsFrontFace\n";

        auto pixelShader = reinterpret_cast<const PixelShader*>(shader);
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += ",\n\tout float4 oC0 : SV_Target0";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += ",\n\tout float4 oC1 : SV_Target1";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += ",\n\tout float4 oC2 : SV_Target2";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += ",\n\tout float4 oC3 : SV_Target3";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += ",\n\tout float oDepth : SV_Depth";
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];


            out += '\t';

            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    print("[[vk::location({})]] ", usageLocation.location);
                    break;
                }
            }

            println("in {0} i{1}{2} : {3}{2},", usageType, USAGE_VARIABLES[uint32_t(vertexElement.usage)],
                uint32_t(vertexElement.usageIndex), USAGE_SEMANTICS[uint32_t(vertexElement.usage)]);

            vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
            elementOrdinal.emplace(uint32_t(vertexElement.address), i);
        }


        out += "\tout float4 oPos : SV_Position";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print(",\n\tout float4 o{0}{1} : {2}{1}", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);
    }

    out += ")\n";
    out += "{\n";


    if (shaderContainer->definitionTableOffset != NULL)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            auto value = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + definition->physicalOffset);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
                println("\tfloat4 c{} = asfloat(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());

                value += 4;
            }
            definitions += 2;
        }
        ++definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Int4Definition*>(definitions);
            for (uint16_t i = 0; i < definition->count; i++)
            {
                /*
                 * Named: GCC does not accept the anonymous local union member
                 * that MSVC and clang accept with -fms-extensions.
                 */
                union Bytes4
                {
                    uint32_t value;
                    struct { int8_t x, y, z, w; } b;
                } u;

                u.value = definition->values[i].get();

                println("\tint4 i{} = int4({}, {}, {}, {});",
                    (definition->registerIndex - 8992) / 4 + i, u.b.x, u.b.y, u.b.z, u.b.w);
            }
            definitions += 2;
            definitions += definition->count;
        }

        out += "\n";
    }

    bool printedRegisters[32]{};

    uint32_t interpolatorCount = (shader->interpolatorInfo >> 5) & 0x1F;

    for (uint32_t i = 0; i < interpolatorCount; i++)
    {
        union
        {
            Interpolator interpolator;
            uint32_t value;
        };
    
        if (isPixelShader)
        {
            value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
            println("\tfloat4 r{} = i{}{};", uint32_t(interpolator.reg), USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex));
            printedRegisters[interpolator.reg] = true;
        }
        else
        {
            auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
            interpolators.emplace(i, fmt::format("o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex)));
        }
    }

    if (!isPixelShader)
    {

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\to{}{} = 0.0;", USAGE_VARIABLES[uint32_t(usage)], usageIndex);

        out += "\n";
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (!printedRegisters[i])
        {
            print("\tfloat4 r{} = ", i);
            if (isPixelShader && i == ((shader->fieldC >> 8) & 0xFF))
            {
                out += "float4((iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
            }
            else
            {
                out += "0.0;\n";
            }
        }
    }

    // Where the GPRs above r31 are declared, once the body says which ones it uses (end of this function).
    const size_t marcaRegistrosAltos = out.size();

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";
    if (isPixelShader)
    {
        out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";
    }

    const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + shader->physicalOffset);

    /*
     * Named, with references: GCC does not accept the anonymous struct inside a local
     * union that MSVC and clang accept. The references leave the rest of the body
     * untouched, which uses code0..code3 eighteen times.
     */
    union ControlFlowWords
    {
        ControlFlowInstruction controlFlow[2];
        struct { uint32_t code0, code1, code2, code3; } w;
    } cfu;
    uint32_t& code0 = cfu.w.code0;
    uint32_t& code1 = cfu.w.code1;
    uint32_t& code2 = cfu.w.code2;
    uint32_t& code3 = cfu.w.code3;

    auto controlFlowCode = code;
    uint32_t instrAddress = 0;
    uint32_t instrSize = shader->size;
    bool simpleControlFlow = true;
    // Forward jumps only (no loop, no jump back): see saltosLineales.
    bool soloHaciaDelante = true;
    uint32_t indiceCf = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : cfu.controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional || cfInstr.condJmp.direction)
                    simpleControlFlow = false;
                else
                    ++ifEndLabels[cfInstr.condJmp.address];

                if (cfInstr.condJmp.address <= indiceCf)
                    soloHaciaDelante = false;
                break;
            }

            case ControlFlowOpcode::LoopStart:
            case ControlFlowOpcode::LoopEnd:
                soloHaciaDelante = false;
                break;
            }

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);
            ++indiceCf;
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    /*
     * saltosLineales: jumps the translator could not nest (an unconditional jump, the end of an `if` branch) are
     * emitted as `while (true) switch (pc)`. When every jump goes forward, that loop is not needed: each control
     * flow instruction becomes `if (pc <= N) { ... }` in order, and a jump only raises pc. pc never goes down,
     * so a block runs exactly when no jump went past it, as in the switch. Without the loop's back edge,
     * SPIRV-Cross does not copy every live value on every edge (Dead Space 2's light shaders came out at ~450 KB of
     * GLSL that uam's register allocator could not finish) and the code is plain nested ifs.
     */
    const bool lineal = !simpleControlFlow && soloHaciaDelante && saltosLineales;

    if (simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
    }
    else if (lineal)
    {
        out += "\n\tuint pc = 0;\n";
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    controlFlowCode = code;
    instrAddress = 0;
    uint32_t pc = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : cfu.controlFlow)
        {
            if (lineal)
            {
                indentation = 3;
                if (pc != 0)
                    out += "\t\t}\n";
                println("\t\tif (pc <= {})", pc);
                out += "\t\t{\n";
            }
            else if (!simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
            }

            ++pc;

            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;
            bool shouldReturn = false;
            bool shouldCloseCurlyBracket = false;
            /*
             * If this CF opens its own `if (p0)` (CondExecPred), its condition. It serves
             * two purposes: reusing the identical block that was just closed, and leaving its
             * own brace recorded so the next one can reuse it. See reabrirCierre().
             */
            int condicionPredicadoCf = -1;

            // Control flow boundary. An open predicated block does not cross this point.
            cerrarPredicado();

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::ExecEnd);
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd);
                {
                    // The condition belongs to the whole EXEC block, including its
                    // return. Per-instruction ALU predication does not replace it.
                    const auto booleano = boolConstants.find(cfInstr.condExec.boolAddress);
                    if (booleano == boolConstants.end())
                        throw std::runtime_error("EXEC condicional sin constante booleana declarada");
                    indent();
                    println("if ((g_Booleans & {}) {}= 0)", booleano->second, cfInstr.condExec.condition ? "!" : "=");
                    indent();
                    out += "{\n";
                    ++indentation;
                    shouldCloseCurlyBracket = true;
                }
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
                condicionPredicadoCf = cfInstr.condExecPred.condition ? 1 : 0;
                /*
                 * The previous block was this same `if (p0)` and nothing was emitted between
                 * the two: its brace is deleted and this EXEC continues inside. This way runs of
                 * CondExecPred with the same predicate (the nine PCF taps, the six car lights)
                 * end up in one basic block instead of three or four.
                 */
                if (!reabrirCierre(condicionPredicadoCf))
                {
                    indent();
                    println("if ({}p0)", condicionPredicadoCf ? "" : "!");
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                shouldCloseCurlyBracket = true;
                // This `if` governs the whole block, so the predicated instructions inside
                // do not have to test it again. See predGarantizado_.
                predGarantizado_ = condicionPredicadoCf;
                break;

            case ControlFlowOpcode::LoopStart:
                if (simpleControlFlow)
                {
                    indent();
                    println("for (aL = 0; aL < i{}.x; aL++)", uint32_t(cfInstr.loopStart.loopId));
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                else 
                {
                    out += "\t\t\taL = 0;\n";
                }
                break;

            case ControlFlowOpcode::LoopEnd:
                if (simpleControlFlow)
                {
                    --indentation;
                    indent();
                    out += "}\n";
                }
                else
                {
                    out += "\t\t\t++aL;\n";
                    println("\t\t\tif (aL < i{}.x)", uint32_t(cfInstr.loopEnd.loopId));
                    out += "\t\t\t{\n";
                    println("\t\t\t\tpc = {};", uint32_t(cfInstr.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                }
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional)
                {
                    assert(!simpleControlFlow);
                    println("\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                    if (!lineal)
                        out += "\t\t\tcontinue;\n";
                }
                else
                {
                    indent();
                    if (cfInstr.condJmp.isPredicated)
                    {
                        println("if ({}p0)", cfInstr.condJmp.condition ^ simpleControlFlow ? "" : "!");
                    }
                    else
                    {
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
                            println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                        else
                            println("if (b{} {}= 0)", uint32_t(cfInstr.condJmp.boolAddress), cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                    }

                    if (simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        if (!lineal)
                            out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }
            }

            auto instructionCode = code + address * 3;
            
            for (uint32_t i = 0; i < count; i++)
            {
                /*
                 * Named, for the same reason as the other two: GCC does not accept
                 * the anonymous struct inside a local union.
                 */
                union InstrWords
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct { uint32_t code0, code1, code2; } w;
                } iu;
                VertexFetchInstruction& vertexFetch = iu.vertexFetch;
                TextureFetchInstruction& textureFetch = iu.textureFetch;
                AluInstruction& alu = iu.alu;

                iu.w.code0 = instructionCode[0];
                iu.w.code1 = instructionCode[1];
                iu.w.code2 = instructionCode[2];
            
                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        recompile(vertexFetch, address + i);
                    }
                    else
                    {
                        {
                            recompile(textureFetch, false);
                        }
                    }
                }
                else
                {
                    recompile(alu);
                }
            
                sequence >>= 2;
                instructionCode += 3;
            }

            cerrarPredicado();  // end of the EXEC instructions

            if (shouldReturn)
            {
                if (isPixelShader)
                {
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TEST;

                    indent();
                    out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)";
                    indent();
                    out += '{';

                    indent();
                    // the 8 comparison functions (alphaTestValue, shader_common.h).
                    out += "\tclip(alphaTestValue(oC0.w));\n";

                    indent();
                    out += "}";

                }
                else
                {
                    {
                        // To host clip space (ndc_scale/ndc_offset from
                        // draw.cpp): draws in pixels with clipping disabled.
                        out += "\toPos.xy = oPos.xy * g_NdcScale + g_NdcOffset * oPos.w;\n";
                        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
                    }
                }

                if (simpleControlFlow)
                {
                    indent();
                    {
                        out += "return;\n";
                    }
                }
                else
                {
                    // The end of the shader: out of the switch and the loop, or simply out of main.
                    out += lineal ? "\t\t\treturn;\n" : "\t\t\tbreak;\n";
                }
            }

            if (shouldCloseCurlyBracket)
            {
                --indentation;
                const size_t marcaLlaveCf = out.size();
                indent();
                out += "}\n";
                /*
                 * It is recorded in case the next CondExecPred is identical. Not with a
                 * `return` inside, since the block does not continue; and anotarCierreCf() also
                 * refuses if something wrote p0 in here (predGarantizado_ was set to -1): the
                 * next block has to test the new p0.
                 */
                anotarCierreCf(marcaLlaveCf, shouldReturn ? -1 : condicionPredicadoCf);
            }
            predGarantizado_ = -1;  // end of the EXEC block: it no longer guarantees anything
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (lineal)
    {
        out += "\t\t}\n";
    }
    else if (!simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }


    /*
     * GPRs above r31. Only r0-r31 are declared up front, but a Xenos shader can use up to r127: Dead Space 2
     * has pixel shaders up to r62. The body is scanned for rN with N >= 32 and those are declared next to the
     * others, zeroed like them. A shader that stays within r31 gets no extra declarations.
     */
    {
        bool usados[128]{};
        bool alguno = false;
        for (size_t i = marcaRegistrosAltos; i + 1 < out.size(); ++i)
        {
            const char c = out[i];
            if (c != 'r' || (i > 0 && (isalnum(static_cast<unsigned char>(out[i - 1])) || out[i - 1] == '_')))
                continue;
            size_t j = i + 1;
            uint32_t n = 0;
            while (j < out.size() && isdigit(static_cast<unsigned char>(out[j])) && n < 1000)
                n = n * 10 + uint32_t(out[j++] - '0');
            if (j == i + 1 || (j < out.size() && (isalnum(static_cast<unsigned char>(out[j])) || out[j] == '_')))
                continue;
            if (n >= 32 && n < 128)
                usados[n] = alguno = true;
        }
        if (alguno)
        {
            std::string declaraciones;
            for (uint32_t n = 32; n < 128; ++n)
                if (usados[n])
                    declaraciones += fmt::format("\tfloat4 r{} = 0.0;\n", n);
            out.insert(marcaRegistrosAltos, declaraciones);
        }
    }

    /*
     * Cube maps the constant table calls 2D. Dead Space 2's g_envSampler is declared sampler2D but fetched as a
     * cube, so its deko3d binding (printDeko3dConstants, typed from the table) is a Texture2D and
     * _TextureCubeDescriptorIndex is never defined. When every fetch of a sampler is a cube and its binding is
     * 2D, the binding becomes a TextureCube and loses the pair (the pair only exists for 2D). Shaders whose
     * table and fetches agree are not touched.
     */
    for (const auto& [nombre, dimensiones] : dimensionesBuscadas)
    {
        if (dimensiones != (1u << uint32_t(TextureDimension::TextureCube)))
            continue;
        const std::string textura2D = fmt::format("]] Texture2D<float4> {}_Textura;\n", nombre);
        const std::string indice2D = fmt::format("#define {0}_Texture2DDescriptorIndex {0}_Textura\n", nombre);
        const size_t posTextura = out.find(textura2D);
        const size_t posIndice = out.find(indice2D);
        if (posTextura == std::string::npos || posIndice == std::string::npos)
            continue;
        out.replace(posIndice, indice2D.size(), fmt::format("#define {0}_TextureCubeDescriptorIndex {0}_Textura\n", nombre));
        out.replace(posTextura, textura2D.size(), fmt::format("]] TextureCube<float4> {}_Textura;\n", nombre));
        const size_t posPareja = out.find(fmt::format("]] Texture2D<float4> {}_Pareja;\n", nombre));
        const std::string definePareja = fmt::format("#define {0}_Texture3DDescriptorIndex {0}_Pareja\n", nombre);
        if (posPareja != std::string::npos)
        {
            const size_t inicio = out.rfind("[[vk::binding(", posPareja);
            const size_t fin = out.find(definePareja, posPareja);
            if (inicio != std::string::npos && fin != std::string::npos)
                out.erase(inicio, fin + definePareja.size() - inicio);
        }
    }

    /*
     * The vertex index in r0.x. A Xenos vertex shader starts with its vertex's index in r0.x (as a float); the pixel
     * shader's equivalent (vPos in its register) is above. Dead Space 2's particle shaders build each quad from it
     * (which corner a vertex is: `r0.y = r0.x * c248.w + c244.w` before anything writes r0), and with r0 = 0 every
     * vertex was corner 0 with garbage offsets: the huge triangles of fire, sparks and dust across the screen.
     * Only shaders that read r0.x before writing it get it, so the others (and the DKSH caches of their SPIR-V) stay
     * as they were. The index is SV_VertexID plus g_IndiceBase: the renderer draws from the first vertex the draw
     * uses (base vertex -vmin) and puts vmin there.
     */
    if (!isPixelShader)
    {
        const std::string declaracion = "\tfloat4 r0 = 0.0;\n";
        const size_t posDeclaracion = out.find(declaracion);
        const size_t cuerpo = out.find("\tfloat ps = 0.0;\n", marcaRegistrosAltos);
        if (posDeclaracion != std::string::npos && cuerpo != std::string::npos)
        {
            static const std::regex asignacion(R"(^\s*r0\.([xyzw]+)\s*=[^=](.*)$)");
            static const std::regex lectura(R"(\br0\.([xyzw]+))");
            bool escritoX = false, leidoX = false;
            size_t inicio = cuerpo;
            while (!leidoX && !escritoX && inicio < out.size())
            {
                size_t fin = out.find('\n', inicio);
                if (fin == std::string::npos) fin = out.size();
                const std::string linea = out.substr(inicio, fin - inicio);
                inicio = fin + 1;
                std::smatch m;
                const bool esAsignacion = std::regex_match(linea, m, asignacion);
                const std::string derecha = esAsignacion ? m[2].str() : linea;
                for (auto it = std::sregex_iterator(derecha.begin(), derecha.end(), lectura);
                     it != std::sregex_iterator(); ++it)
                {
                    if ((*it)[1].str().find('x') != std::string::npos) leidoX = true;
                }
                if (esAsignacion && m[1].str().find('x') != std::string::npos) escritoX = true;
            }
            if (leidoX)
            {
                out.replace(posDeclaracion, declaracion.size(),
                    "\tfloat4 r0 = float4(float(iVertexId + g_IndiceBase), 0.0, 0.0, 0.0);\n");
                const size_t main = out.find("void main(\n");
                if (main != std::string::npos)
                    out.insert(main + 11, "\tin uint iVertexId : SV_VertexID,\n");
            }
        }
    }

    /*
     * Texture fetches in vertex shaders (Dead Space 2's modulate_sampler_vs). tfetch2D of shader_common.h uses
     * Sample, which needs derivatives and is only allowed in pixel and compute shaders. tfetch2DVertice is the
     * same with SampleLevel at mip 0. It is written into the shader itself, right after shader_common.h, only
     * when the shader uses it.
     */
    if (usaLecturaManual)
    {
        out.insert(marcaTrasInclude, kLecturaManualHlsl);
    }
    if (usaTfetchVertice)
    {
        out.insert(marcaTrasInclude,
            "float4 tfetch2DVertice(Texture2D<float4> texture, SamplerState samplerState, float2 texCoord, float2 offset, float2 invSize)\n"
            "{\n"
            "    float2 desplazamiento;\n"
            "    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TAMANO_TEX)\n"
            "        desplazamiento = offset * invSize;\n"
            "    else\n"
            "        desplazamiento = offset / getTexture2DDimensions(texture);\n"
            "    return texture.SampleLevel(samplerState, texCoord + desplazamiento, 0.0);\n"
            "}\n"
            "\n");
    }

    out += "}";
}
