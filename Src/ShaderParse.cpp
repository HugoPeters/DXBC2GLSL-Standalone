/**
 * @file ShaderParse.cpp
 * @author Shenghua Lin, Minmin Gong, Luca Barbieri
 *
 * @section DESCRIPTION
 *
 * This source file is part of KlayGE
 * For the latest info, see http://www.klayge.org
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * You may alternatively use this source under the terms of
 * the KlayGE Proprietary License (KPL). You can obtained such a license
 * from http://www.klayge.org/licensing/.
 */

#include <DXBC2GLSL/Shader.hpp>
#include <DXBC2GLSL/Utils.hpp>
#include <algorithm>

namespace
{
#ifdef KLAYGE_HAS_STRUCT_PACK
#pragma pack(push, 1)
#endif
	struct DXBCSignatureParameterD3D10
	{
		uint32_t name_offset;
		uint32_t semantic_index;
		uint32_t system_value_type;
		uint32_t component_type;
		uint32_t register_num;
		uint8_t mask;
		uint8_t read_write_mask;
		uint8_t padding[2];
	};

	struct DXBCSignatureParameterD3D11
	{
		uint32_t stream;
		uint32_t name_offset;
		uint32_t semantic_index;
		uint32_t system_value_type;
		uint32_t component_type;
		uint32_t register_num;
		uint8_t mask;
		uint8_t read_write_mask;
		uint8_t padding[2];
	};

	struct DXBCSignatureParameterD3D11_1
	{
		uint32_t stream;
		uint32_t name_offset;
		uint32_t semantic_index;
		uint32_t system_value_type;
		uint32_t component_type;
		uint32_t register_num;
		uint8_t mask;
		uint8_t read_write_mask;
		uint8_t padding[2];
		uint32_t min_precision;
	};
#ifdef KLAYGE_HAS_STRUCT_PACK
#pragma pack(pop)
#endif

	bool SortFuncLess(DXBCShaderVariable const & lh, DXBCShaderVariable const & rh)
	{
		return lh.var_desc.start_offset < rh.var_desc.start_offset;
	}
}

struct ShaderParser
{
	uint32_t const * tokens;//shader tokens
	uint32_t const * tokens_end;
	DXBCChunkHeader const * resource_chunk;//resource definition and constant buffer chunk
	DXBCChunkSignatureHeader const * input_signature;
	DXBCChunkSignatureHeader const * output_signature;
	DXBCChunkSignatureHeader const * patch_constant_signature;
	std::shared_ptr<ShaderProgram> program;

	ShaderParser(const DXBCContainer& dxbc, std::shared_ptr<ShaderProgram> const & program)
		: program(program)
	{
		resource_chunk = dxbc.resource_chunk;
		input_signature = reinterpret_cast<DXBCChunkSignatureHeader const *>(dxbc.input_signature);
		output_signature = reinterpret_cast<DXBCChunkSignatureHeader const *>(dxbc.output_signature);
		patch_constant_signature = reinterpret_cast<DXBCChunkSignatureHeader const *>(dxbc.patch_constant_signature);
		uint32_t size = le2native(dxbc.shader_chunk->size);
		tokens = reinterpret_cast<uint32_t const *>(dxbc.shader_chunk + 1);
		tokens_end = reinterpret_cast<uint32_t const *>(reinterpret_cast<char const *>(tokens) + size);
	}

	uint32_t Read32()
	{
		assert(tokens < tokens_end);
		uint32_t cur_token = le2native(*tokens);
		++ tokens; 
		return cur_token;
	}

	template <typename T>
	void ReadToken(T* tok)
	{
		*reinterpret_cast<uint32_t*>(tok) = this->Read32();
	}

	uint64_t Read64()
	{
		uint32_t a = this->Read32();
		uint32_t b = this->Read32();
		return static_cast<uint64_t>(a) | (static_cast<uint64_t>(b) << 32);
	}

	void Skip(uint32_t toskip)
	{
		tokens += toskip;
	}

	void ReadOp(ShaderOperand& op)
	{
		TokenizedShaderOperand optok;
		this->ReadToken(&optok);
		assert(optok.op_type < SOT_COUNT);
		op.swizzle[0] = 0;
		op.swizzle[1] = 1;
		op.swizzle[2] = 2;
		op.swizzle[3] = 3;
		op.mask = 0xF;
		switch (optok.comps_enum)
		{
		case SONC_0:
			op.comps = 0;
			break;

		case SONC_1:
			op.comps = 1;
			op.swizzle[1] = op.swizzle[2] = op.swizzle[3] = 0;
			break;

		case SONC_4:
			op.comps = 4;
			op.mode = optok.mode;
			switch (optok.mode)
			{
			case SOSM_MASK:
				op.mask = SM_OPERAND_SEL_MASK(optok.sel);
				break;

			case SOSM_SWIZZLE:
				op.swizzle[0] = SM_OPERAND_SEL_SWZ(optok.sel, 0);
				op.swizzle[1] = SM_OPERAND_SEL_SWZ(optok.sel, 1);
				op.swizzle[2] = SM_OPERAND_SEL_SWZ(optok.sel, 2);
				op.swizzle[3] = SM_OPERAND_SEL_SWZ(optok.sel, 3);
				break;

			case SOSM_SCALAR:
				op.swizzle[0] = op.swizzle[1] = op.swizzle[2] = op.swizzle[3] = SM_OPERAND_SEL_SCALAR(optok.sel);
				break;

			default:
				assert(false);
				break;
			}
			break;

		default:
			assert_msg(false, "Unhandled operand component type");
			break;
		}
		op.type = static_cast<ShaderOperandType>(optok.op_type);
		op.num_indices = optok.num_indices;

		if (optok.extended)
		{
			TokenizedShaderOperandExtended optokext;
			this->ReadToken(&optokext);
			if (0 == optokext.type)
			{
			}
			else if (1 == optokext.type)
			{
				op.neg = optokext.neg;
				op.abs = optokext.abs;
			}
			else
			{
				assert_msg(false, "Unhandled extended operand token type");
			}
		}

		for (uint32_t i = 0; i < op.num_indices; ++ i)
		{
			uint32_t repr;
			if (0 == i)
			{
				repr = optok.index0_repr;
			}
			else if (1 == i)
			{
				repr = optok.index1_repr;
			}
			else if (2 == i)
			{
				repr = optok.index2_repr;
			}
			else
			{
				assert_msg(false, "Unhandled operand index representation");
				repr = 0;
			}
			op.indices[i].disp = 0;
			// TODO: is disp supposed to be signed here??
			switch (repr)
			{
			case SOIP_IMM32:
				op.indices[i].disp = static_cast<int32_t>(this->Read32());
				break;

			case SOIP_IMM64:
				op.indices[i].disp = this->Read64();
				break;

			case SOIP_RELATIVE:
				op.indices[i].reg = std::make_shared<ShaderOperand>();
				this->ReadOp(*op.indices[i].reg);
				break;

			case SOIP_IMM32_PLUS_RELATIVE:
				op.indices[i].disp = static_cast<int32_t>(this->Read32());
				op.indices[i].reg = std::make_shared<ShaderOperand>();
				this->ReadOp(*op.indices[i].reg);
				break;

			case SOIP_IMM64_PLUS_RELATIVE:
				op.indices[i].disp = this->Read64();
				op.indices[i].reg = std::make_shared<ShaderOperand>();
				this->ReadOp(*op.indices[i].reg);
				break;
			}
		}

		if (SOT_IMMEDIATE32 == op.type)
		{
			for (uint32_t i = 0; i < op.comps; ++ i)
			{
				op.imm_values[i].i32 = this->Read32();
			}
		}
		else if (SOT_IMMEDIATE64 == op.type)
		{
			for (uint32_t i = 0; i < op.comps; ++ i)
			{
				op.imm_values[i].i64 = this->Read64();
			}
		}
	}

	void ParseShader()
	{
		this->ReadToken(&program->version);

		uint32_t lentok = this->Read32();
		tokens_end = tokens - 2 + lentok;

		uint32_t cur_gs_stream = 0;

		while (tokens != tokens_end)
		{
			TokenizedShaderInstruction insntok;
			this->ReadToken(&insntok);
			uint32_t const * insn_end = tokens - 1 + insntok.length;
			ShaderOpcode opcode = insntok.opcode;
			assert(opcode < SO_COUNT);

			if (SO_IMMEDIATE_CONSTANT_BUFFER == opcode)
			{
				// immediate constant buffer data
				uint32_t customlen = this->Read32() - 2;

				std::shared_ptr<ShaderDecl> dcl = std::make_shared<ShaderDecl>();
				program->dcls.push_back(dcl);

				dcl->opcode = SO_IMMEDIATE_CONSTANT_BUFFER;
				dcl->num = customlen;
				dcl->data.resize(customlen * sizeof(tokens[0]));

				memcpy(&dcl->data[0], &tokens[0], customlen * sizeof(tokens[0]));

				this->Skip(customlen);
				continue;
			}

			if ((SO_HS_FORK_PHASE == opcode) || (SO_HS_JOIN_PHASE == opcode) || (SO_HS_CONTROL_POINT_PHASE == opcode) || (SO_HS_DECLS == opcode ))
			{
				// need to interleave these with the declarations or we cannot
				// assign fork/join phase instance counts to phases
				std::shared_ptr<ShaderDecl> dcl = std::make_shared<ShaderDecl>();
				program->dcls.push_back(dcl);
				dcl->opcode = opcode;
			}

			if (((opcode >= SO_DCL_RESOURCE) && (opcode <= SO_DCL_GLOBAL_FLAGS))
				|| ((opcode >= SO_DCL_STREAM) && (opcode <= SO_DCL_RESOURCE_STRUCTURED))
				|| (SO_DCL_GS_INSTANCE_COUNT == opcode))
			{
				std::shared_ptr<ShaderDecl> dcl = std::make_shared<ShaderDecl>();
				program->dcls.push_back(dcl);
				reinterpret_cast<TokenizedShaderInstruction&>(*dcl) = insntok;

				TokenizedShaderInstructionExtended exttok;
				memcpy(&exttok, &insntok, sizeof(exttok));
				while(exttok.extended)
				{
					this->ReadToken(&exttok);
				}

#define READ_OP_ANY dcl->op = std::make_shared<ShaderOperand>(); this->ReadOp(*dcl->op);
#define READ_OP(FILE) READ_OP_ANY
				//check(dcl->op->file == SOT_##FILE);

				switch (opcode)
				{
				case SO_DCL_GLOBAL_FLAGS:
					break;

				case SO_DCL_RESOURCE:
					READ_OP(RESOURCE);
					this->ReadToken(&dcl->rrt);
					break;

				case SO_DCL_SAMPLER:
					READ_OP(SAMPLER);
					break;

				case SO_DCL_INPUT:
				case SO_DCL_INPUT_PS:
					READ_OP(INPUT);
					break;

				case SO_DCL_INPUT_SIV:
				case SO_DCL_INPUT_SGV:
				case SO_DCL_INPUT_PS_SIV:
				case SO_DCL_INPUT_PS_SGV:
					READ_OP(INPUT);
					dcl->sv = static_cast<ShaderSystemValue>(static_cast<uint16_t>(this->Read32()));
					break;

				case SO_DCL_OUTPUT:
					READ_OP(OUTPUT);
					break;

				case SO_DCL_OUTPUT_SIV:
				case SO_DCL_OUTPUT_SGV:
					READ_OP(OUTPUT);
					dcl->sv = static_cast<ShaderSystemValue>(static_cast<uint16_t>(this->Read32()));
					break;

				case SO_DCL_INDEX_RANGE:
					READ_OP_ANY;
					assert((SOT_INPUT == dcl->op->type) || (SOT_OUTPUT == dcl->op->type));
					dcl->num = this->Read32();
					break;

				case SO_DCL_TEMPS:
					dcl->num = this->Read32();
					break;

				case SO_DCL_INDEXABLE_TEMP:
					dcl->op = std::make_shared<ShaderOperand>();
					dcl->op->indices[0].disp = this->Read32();
					dcl->indexable_temp.num = this->Read32();
					dcl->indexable_temp.comps = this->Read32();
					break;

				case SO_DCL_CONSTANT_BUFFER:
					READ_OP(CONSTANT_BUFFER);
					break;

				case SO_DCL_GS_INPUT_PRIMITIVE:
					program->gs_input_primitive = dcl->dcl_gs_input_primitive.primitive;
					break;

				case SO_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
					program->gs_output_topology[cur_gs_stream]
						= dcl->dcl_gs_output_primitive_topology.primitive_topology;
					break;

				case SO_DCL_MAX_OUTPUT_VERTEX_COUNT:
					dcl->num = this->Read32();
					program->max_gs_output_vertex = dcl->num;
					break;

				case SO_DCL_GS_INSTANCE_COUNT:
					dcl->num = this->Read32();
					program->gs_instance_count = dcl->num;
					break;
				
				case SO_DCL_TESS_OUTPUT_PRIMITIVE:
					program->ds_tessellator_output_primitive
						= static_cast<ShaderTessellatorOutputPrimitive>(dcl->dcl_tess_output_primitive.primitive & 0x7);
					break;

				case SO_DCL_TESS_PARTITIONING:
					program->ds_tessellator_partitioning
						= static_cast<ShaderTessellatorPartitioning>(dcl->dcl_tess_partitioning.partitioning & 0x7);
					break;

				case SO_DCL_TESS_DOMAIN:
					program->ds_tessellator_domain = static_cast<ShaderTessellatorDomain>(dcl->dcl_tess_domain.domain & 0x7);
					break;

				case SO_DCL_OUTPUT_CONTROL_POINT_COUNT:
					program->hs_output_control_point_count = dcl->dcl_output_control_point_count.control_points;
					break;

				case SO_DCL_INPUT_CONTROL_POINT_COUNT:
					program->hs_input_control_point_count = dcl->dcl_input_control_point_count.control_points;
					break;

				case SO_DCL_HS_MAX_TESSFACTOR:
					dcl->num = this->Read32();
					break;

				case SO_DCL_HS_FORK_PHASE_INSTANCE_COUNT:
					dcl->num = this->Read32();
					break;

				case SO_DCL_FUNCTION_BODY:
					dcl->num = this->Read32();
					break;

				case SO_DCL_FUNCTION_TABLE:
					dcl->num = this->Read32();
					dcl->data.resize(dcl->num * sizeof(uint32_t));
					for (uint32_t i = 0; i < dcl->num; ++ i)
					{
						(reinterpret_cast<uint32_t*>(&dcl->data[0]))[i] = this->Read32();
					}
					break;

				case SO_DCL_INTERFACE:
					dcl->intf.id = this->Read32();
					dcl->intf.expected_function_table_length = this->Read32();
					{
						uint32_t v = this->Read32();
						dcl->intf.table_length = v & 0xffff;
						dcl->intf.array_length = v >> 16;
					}
					dcl->data.resize(dcl->intf.table_length * sizeof(uint32_t));
					for (uint32_t i = 0; i < dcl->intf.table_length; ++ i)
					{
						(reinterpret_cast<uint32_t*>(&dcl->data[0]))[i] = this->Read32();
					}
					break;

				case SO_DCL_THREAD_GROUP:
					dcl->thread_group_size[0] = this->Read32();
					dcl->thread_group_size[1] = this->Read32();
					dcl->thread_group_size[2] = this->Read32();
					program->cs_thread_group_size[0] = dcl->thread_group_size[0];
					program->cs_thread_group_size[1] = dcl->thread_group_size[1];
					program->cs_thread_group_size[2] = dcl->thread_group_size[2];
					break;

				case SO_DCL_UNORDERED_ACCESS_VIEW_TYPED:
					READ_OP(UNORDERED_ACCESS_VIEW);
					this->ReadToken(&dcl->rrt);
					break;

				case SO_DCL_UNORDERED_ACCESS_VIEW_RAW:
					READ_OP(UNORDERED_ACCESS_VIEW);
					break;

				case SO_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED:
					READ_OP(UNORDERED_ACCESS_VIEW);
					dcl->structured.stride = this->Read32();
					break;

				case SO_DCL_THREAD_GROUP_SHARED_MEMORY_RAW:
					READ_OP(THREAD_GROUP_SHARED_MEMORY);
					dcl->num = this->Read32();
					break;

				case SO_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED:
					READ_OP(THREAD_GROUP_SHARED_MEMORY);
					dcl->structured.stride = this->Read32();
					dcl->structured.count = this->Read32();
					break;

				case SO_DCL_RESOURCE_RAW:
					READ_OP(RESOURCE);
					break;

				case SO_DCL_RESOURCE_STRUCTURED:
					READ_OP(RESOURCE);
					dcl->structured.stride = this->Read32();
					break;

				case SO_DCL_STREAM:
					READ_OP(STREAM);
					cur_gs_stream = static_cast<uint32_t>(dcl->op->indices[0].disp);
					program->gs_output_topology.push_back(SPT_Undefined);
					break;

				default:
					assert_msg(false, "Unhandled declaration");
					break;
				}

				assert(tokens == insn_end);
			}
			else
			{
				if (SO_HS_DECLS == opcode)
				{
					continue;
				}
				std::shared_ptr<ShaderInstruction> insn = std::make_shared<ShaderInstruction>();
				program->insns.push_back(insn);
				reinterpret_cast<TokenizedShaderInstruction&>(*insn) = insntok;

				TokenizedShaderInstructionExtended exttok;
				memcpy(&exttok, &insntok, sizeof(exttok));
				while (exttok.extended)
				{
					this->ReadToken(&exttok);
					if (SEOP_SAMPLE_CONTROLS == exttok.type)
					{
						insn->sample_offset[0] = exttok.sample_controls.offset_u;
						insn->sample_offset[1] = exttok.sample_controls.offset_v;
						insn->sample_offset[2] = exttok.sample_controls.offset_w;
					}
					else if (SEOP_RESOURCE_DIM == exttok.type)
					{
						insn->resource_target = exttok.resource_target.target;
					}
					else if (SEOP_RESOURCE_RETURN_TYPE == exttok.type)
					{
						insn->resource_return_type[0] = exttok.resource_return_type.x;
						insn->resource_return_type[1] = exttok.resource_return_type.y;
						insn->resource_return_type[2] = exttok.resource_return_type.z;
						insn->resource_return_type[3] = exttok.resource_return_type.w;
					}
				}

				switch (opcode)
				{
				case SO_INTERFACE_CALL:
					insn->num = this->Read32();
					break;

				default:
					break;
				}

				uint32_t op_num = 0;
				while (tokens != insn_end)
				{
					assert(tokens < insn_end);
					assert(op_num < SM_MAX_OPS);
					insn->ops[op_num] = std::make_shared<ShaderOperand>();
					this->ReadOp(*insn->ops[op_num]);
					++ op_num;
				}
				insn->num_ops = op_num;
			}
		}
	}

	char const * Parse()
	{
		this->ParseShader();
		
		if (resource_chunk)
		{
			this->ParseCBAndResourceBinding();
			this->SortCBVars();
		}
		
		if (input_signature)
		{
			if (FOURCC_ISG1 == input_signature->fourcc)
			{
				this->ParseSignature(input_signature, FOURCC_ISG1);
			}
			else
			{
				assert(FOURCC_ISGN == input_signature->fourcc);
				this->ParseSignature(input_signature, FOURCC_ISGN);
			}
		}

		if (output_signature)
		{
			if (FOURCC_OSG1 == output_signature->fourcc)
			{
				this->ParseSignature(output_signature, FOURCC_OSG1);
			}
			else if (FOURCC_OSG5 == output_signature->fourcc)
			{
				this->ParseSignature(output_signature, FOURCC_OSG5);
			}
			else
			{
				assert(FOURCC_OSGN == output_signature->fourcc);
				this->ParseSignature(output_signature, FOURCC_OSGN);
			}
		}

		if (patch_constant_signature)
		{
			this->ParseSignature(patch_constant_signature, FOURCC_PCSG);
		}

		return nullptr;
	}

	uint32_t ParseCBAndResourceBinding() const
	{
		assert_msg(FOURCC_RDEF == resource_chunk->fourcc, "parameter chunk is not a resource chunk,parse_constant_buffer()");

		uint32_t const * res_token = reinterpret_cast<uint32_t const *>(resource_chunk + 1);
		uint32_t const * first_token = res_token;
		uint32_t num_cb = le2native(*res_token);
		++ res_token;
		uint32_t cb_offset = le2native(*res_token);
		++ res_token;

		uint32_t num_resource_bindings = le2native(*res_token);
		++ res_token;
		uint32_t resource_binding_offset = le2native(*res_token);
		++ res_token;
		uint32_t shader_model = le2native(*res_token);
		++ res_token;
		// TODO: check here, shader_model is unused.
		unused(shader_model);
		uint32_t compile_flags = le2native(*res_token);
		++ res_token;
		// TODO: check here, compile_flags is unused.
		unused(compile_flags);

		uint32_t const * resource_binding_tokens = reinterpret_cast<uint32_t const *>(reinterpret_cast<char const *>(first_token) + resource_binding_offset);
		program->resource_bindings.resize(num_resource_bindings);
		for (uint32_t i = 0; i < num_resource_bindings; ++ i)
		{
			DXBCInputBindDesc& bind = program->resource_bindings[i];
			uint32_t name_offset = le2native(*resource_binding_tokens);
			++ resource_binding_tokens;
			bind.name = reinterpret_cast<char const *>(first_token) + name_offset;
			bind.type = le2native(static_cast<ShaderInputType>(*resource_binding_tokens));
			++ resource_binding_tokens;
			bind.return_type = le2native(static_cast<ShaderResourceReturnType>(*resource_binding_tokens));
			++ resource_binding_tokens;
			bind.dimension = le2native(static_cast<ShaderSRVDimension>(*resource_binding_tokens));
			++ resource_binding_tokens;
			bind.num_samples = le2native(*resource_binding_tokens);
			++ resource_binding_tokens;
			bind.bind_point = le2native(*resource_binding_tokens);
			++ resource_binding_tokens;
			bind.bind_count = le2native(*resource_binding_tokens);
			++ resource_binding_tokens;
			bind.flags = le2native(*resource_binding_tokens);
			++ resource_binding_tokens;
		}

		uint32_t const * cb_tokens = reinterpret_cast<uint32_t const *>(reinterpret_cast<char const *>(first_token) + cb_offset);
		program->cbuffers.resize(num_cb);

		for (uint32_t i = 0; i < num_cb; ++ i)
		{
			DXBCConstantBuffer& cb = program->cbuffers[i];
			uint32_t cb_name_offset = le2native(*cb_tokens);
			++ cb_tokens;
			uint32_t var_count = le2native(*cb_tokens);
			++ cb_tokens;
			uint32_t var_offset = le2native(*cb_tokens);
			++ cb_tokens;
			const uint32_t* var_token = reinterpret_cast<uint32_t const *>(reinterpret_cast<char const *>(first_token) + var_offset);
			cb.vars.resize(var_count);
			for (uint32_t j = 0; j < var_count; ++ j)
			{
				DXBCShaderVariable& var = cb.vars[j];
				uint32_t var_name_offset = le2native(*var_token);
				++ var_token;
				var.var_desc.name = reinterpret_cast<char const *>(first_token) + var_name_offset;
				var.var_desc.start_offset = le2native(*var_token);
				++ var_token;
				var.var_desc.size = le2native(*var_token);
				++ var_token;
				var.var_desc.flags = le2native(*var_token);
				++ var_token;
				uint32_t type_offset = le2native(*var_token);
				++ var_token;
				uint32_t default_value_offset = le2native(*var_token);
				++ var_token;

				if (program->version.major >= 5)
				{
					var.var_desc.start_texture = le2native(*var_token);
					++ var_token;
					var.var_desc.texture_size = le2native(*var_token);
					++ var_token;
					var.var_desc.start_sampler = le2native(*var_token);
					++ var_token;
					var.var_desc.sampler_size = le2native(*var_token);
					++ var_token;

				}
				if (default_value_offset)
				{
					var.var_desc.default_val = reinterpret_cast<char const *>(first_token) + default_value_offset;
				}
				else
				{
					var.var_desc.default_val = nullptr;
				}
				if (type_offset)
				{
					var.has_type_desc = true;
					uint16_t const * type_token = reinterpret_cast<uint16_t const *>(reinterpret_cast<char const *>(first_token) + type_offset);

					var.type_desc.var_class = static_cast<ShaderVariableClass>(le2native(*type_token));
					++ type_token;

					var.type_desc.type = static_cast<ShaderVariableType>(le2native(*type_token));
					++ type_token;

					var.type_desc.rows = le2native(*type_token);
					++ type_token;
					var.type_desc.columns = le2native(*type_token);
					++ type_token;

					var.type_desc.elements = le2native(*type_token);
					++ type_token;
					var.type_desc.members = le2native(*type_token);
					++ type_token;

					uint32_t var_member_offset = le2native(*type_token) << 16;
					++ type_token;
					var_member_offset |= le2native(*type_token);
					++ type_token;
					
					var.type_desc.offset = var_member_offset;
					var.type_desc.name = ShaderVariableTypeName(var.type_desc.type);
				}
				else
				{
					var.has_type_desc = false;
				}
			}

			cb.desc.name = reinterpret_cast<char const *>(first_token) + cb_name_offset;
			cb.desc.size = *cb_tokens;
			++ cb_tokens;
			cb.desc.flags = *cb_tokens;
			++ cb_tokens;
			cb.desc.type = static_cast<ShaderCBufferType>(*cb_tokens);
			++ cb_tokens;
			cb.desc.variables = var_count;
			cb.bind_point = this->GetCBBindPoint(cb.desc.name);
		}

		return num_cb;
	}

	uint32_t GetCBBindPoint(char const * name) const
	{
		for (auto const & ibd : program->resource_bindings)
		{
			if (0 == strcmp(ibd.name, name))
			{
				return ibd.bind_point;
			}
		}

		assert(false);
		return static_cast<uint32_t>(-1);
	}

	uint32_t ParseSignature(DXBCChunkSignatureHeader const * sig, uint32_t fourcc) const
	{
		std::vector<DXBCSignatureParamDesc>* params = nullptr;
		switch (fourcc)
		{
		case FOURCC_ISG1:
		case FOURCC_ISGN:
			params = &program->params_in;
			break;

		case FOURCC_OSG1:
		case FOURCC_OSG5:
		case FOURCC_OSGN:
			params = &program->params_out;
			break;

		case FOURCC_PCSG:
			params = &program->params_patch;
			break;

		default:
			assert(false);
			break;
		}

		uint32_t offset = le2native(sig->offset);
		void const * elements_base
			= static_cast<void const *>(reinterpret_cast<uint8_t const *>(sig)
				+ sizeof(DXBCChunkHeader) + offset);

		uint32_t count = le2native(sig->count);
		params->resize(count);

		if ((FOURCC_ISG1 == fourcc) || (FOURCC_OSG1 == fourcc))
		{
			DXBCSignatureParameterD3D11_1 const * elements
				= reinterpret_cast<DXBCSignatureParameterD3D11_1 const *>(elements_base);
			for (uint32_t i = 0; i < count; ++ i)
			{
				DXBCSignatureParamDesc& param = (*params)[i];
				uint32_t name_offset = le2native(elements[i].name_offset);
				param.semantic_name = reinterpret_cast<char const *>(sig) + sizeof(DXBCChunkHeader) + name_offset;
				param.semantic_index = le2native(elements[i].semantic_index);
				param.system_value_type = le2native(static_cast<ShaderName>(elements[i].system_value_type));
				param.component_type = le2native(static_cast<ShaderRegisterComponentType>(elements[i].component_type));
				param.register_index = le2native(elements[i].register_num);
				param.mask = elements[i].mask;
				param.read_write_mask = elements[i].read_write_mask;
				param.stream = le2native(elements[i].stream);
				param.min_precision = le2native(elements[i].min_precision);
			}
		}
		else if (FOURCC_OSG5 == fourcc)
		{
			DXBCSignatureParameterD3D11 const * elements
				= reinterpret_cast<DXBCSignatureParameterD3D11 const *>(elements_base);
			for (uint32_t i = 0; i < count; ++ i)
			{
				DXBCSignatureParamDesc& param = (*params)[i];
				uint32_t name_offset = le2native(elements[i].name_offset);
				param.semantic_name = reinterpret_cast<char const *>(sig) + sizeof(DXBCChunkHeader) + name_offset;
				param.semantic_index = le2native(elements[i].semantic_index);
				param.system_value_type = le2native(static_cast<ShaderName>(elements[i].system_value_type));
				param.component_type = le2native(static_cast<ShaderRegisterComponentType>(elements[i].component_type));
				param.register_index = le2native(elements[i].register_num);
				param.mask = elements[i].mask;
				param.read_write_mask = elements[i].read_write_mask;
				param.stream = le2native(elements[i].stream);
				param.min_precision = le2native(0);
			}
		}
		else
		{
			assert((FOURCC_ISGN == fourcc) || (FOURCC_OSGN == fourcc) || (FOURCC_PCSG == fourcc));

			DXBCSignatureParameterD3D10 const * elements
				= reinterpret_cast<DXBCSignatureParameterD3D10 const *>(elements_base);
			for (uint32_t i = 0; i < count; ++ i)
			{
				DXBCSignatureParamDesc& param = (*params)[i];
				uint32_t name_offset = le2native(elements[i].name_offset);
				param.semantic_name = reinterpret_cast<char const *>(sig) + sizeof(DXBCChunkHeader) + name_offset;
				param.semantic_index = le2native(elements[i].semantic_index);
				param.system_value_type = le2native(static_cast<ShaderName>(elements[i].system_value_type));
				param.component_type = le2native(static_cast<ShaderRegisterComponentType>(elements[i].component_type));
				param.register_index = le2native(elements[i].register_num);
				param.mask = elements[i].mask;
				param.read_write_mask = elements[i].read_write_mask;
				param.stream = le2native(0);
				param.min_precision = le2native(0);
			}
		}
		
		return count;
	}

	void SortCBVars()
	{
		for (auto& cb : program->cbuffers)
		{
			if (SCBT_CBUFFER == cb.desc.type)
			{
				std::sort(cb.vars.begin(), cb.vars.end(), SortFuncLess);
			}
		}
	}
};

std::shared_ptr<ShaderProgram> ShaderParse(DXBCContainer const & dxbc)
{
	std::shared_ptr<ShaderProgram> program = std::make_shared<ShaderProgram>();
	ShaderParser parser(dxbc, program);
	if (!parser.Parse())
	{
		return program;
	}
	
	return std::shared_ptr<ShaderProgram>();
}
