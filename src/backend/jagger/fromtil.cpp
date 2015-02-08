#include "types.h"
#include "til/til.h"
#include "til/Global.h"
#include "til/VisitCFG.h"

namespace jagger {
namespace {
struct ModuleBuilder {
  struct BlockSidecar {
    ohmu::til::BasicBlock* basicBlock;
    uint entryBlockID;
    uint firstPredecessor;
    uint firstSuccessor;
    uint boundSuccessor;
    Range literals;
  };
  ModuleBuilder(wax::Module& module, ohmu::til::Global& global)
      : module(module), global(global) {
  }
  ModuleBuilder(const ModuleBuilder&) = delete;
  ModuleBuilder& operator =(const ModuleBuilder&) = delete;
  void walkTILGraph();
  void buildFunctionArray();
  void buildBlockSidecarArray();
  void buildBlockArray();
  void countLiterals();
  void buildLiteralsArray();
  void countEvents();
  void buildEventsArray();
  wax::Module& module;
  ohmu::til::Global& global;
  ohmu::til::VisitCFG visitCFG;
  Array<BlockSidecar> blockSidecarArray;
  Array<ohmu::til::Literal*> literals;
};

void ModuleBuilder::walkTILGraph() {
  visitCFG.traverseAll(global.global());
  if (visitCFG.cfgs().empty())
    error("Can't build a module without any input.");
}

void ModuleBuilder::buildFunctionArray() {
  module.functionArray = Array<wax::Function>(visitCFG.cfgs().size());
  auto i = module.functionArray.begin();
  for (auto cfg : visitCFG.cfgs()) i++->blocks.bound = cfg->numBlocks();
  module.functionArray[0].blocks.first = 0;
  for (auto& fun : module.functionArray.slice(1, -1))
    fun.blocks.bound += fun.blocks.first = (&fun)[-1].blocks.bound;
}

void ModuleBuilder::buildBlockSidecarArray() {
  auto& functionArray = module.functionArray;
  blockSidecarArray = Array<BlockSidecar>(functionArray.last().blocks.bound);
  auto function = functionArray.begin();
  auto origin = blockSidecarArray.begin();
  for (auto cfg : visitCFG.cfgs()) {
    uint entryBlockID = function++->blocks.first;
    for (auto& sidecar : blockSidecarArray) {
      auto basicBlock = cfg->blocks()[&sidecar - (origin + entryBlockID)].get();
      sidecar.entryBlockID = entryBlockID;
      sidecar.basicBlock = basicBlock;
      sidecar.firstSuccessor = basicBlock->predecessors().size();
      sidecar.boundSuccessor = basicBlock->successors().size();
    }
  }
  blockSidecarArray[0].firstPredecessor = 0;
  blockSidecarArray[0].boundSuccessor += blockSidecarArray[0].firstSuccessor;
  for (auto& sidecar : blockSidecarArray.slice(1, -1))
    sidecar.boundSuccessor += sidecar.firstSuccessor +=
        sidecar.firstPredecessor = (&sidecar)[-1].boundSuccessor;
}

void ModuleBuilder::buildBlockArray() {
  module.blockArray = Array<wax::Block>(blockSidecarArray.size());
  module.neighborArray =
      Array<uint>(blockSidecarArray.last().boundSuccessor);
  auto sidecar = blockSidecarArray.begin();
  auto blocks = module.blockArray.begin();
  auto neighbors = module.neighborArray.begin();
  for (auto& block : module.blockArray) {
    block.predecessors.first = sidecar->firstPredecessor;
    block.predecessors.bound = sidecar->firstSuccessor;
    block.successors.first = sidecar->firstSuccessor;
    block.successors.bound = sidecar->boundSuccessor;
    block.blockID = INVALID_INDEX;

    auto entryBlockID = sidecar->entryBlockID;
    auto succs = block.successors.bound;
    for (auto& tilSucc : sidecar->basicBlock->successors())
      neighbors[--succs] = entryBlockID + tilSucc->blockID();
    auto preds = block.predecessors.bound;
    for (auto& tilPred : sidecar->basicBlock->predecessors())
      neighbors[--preds] = entryBlockID + tilPred->blockID();

    if (!block.predecessors.size())
      block.caseIndex = INVALID_INDEX;
    if (!block.successors.size())
      block.phiIndex = INVALID_INDEX;
    for (auto& i : block.successors(neighbors))
      blocks[i].caseIndex = (uint)(&i - (neighbors + succs));
    for (auto& i : block.predecessors(neighbors))
      blocks[i].phiIndex = (uint)(&i - (neighbors + preds));

    sidecar++;
  }
}

//==============================================================================
// Counting Literals
//==============================================================================

size_t countBlockLiterals(ohmu::til::BasicBlock& basicBlock) {
  size_t count = 0;
  for (auto instr : basicBlock.instructions()) switch (instr->opcode()) {
      case ohmu::til::COP_Load: { /*TODO: figure out address literals*/
      } break;
      case ohmu::til::COP_UnaryOp: {
        auto unaryOp = ohmu::cast<ohmu::til::UnaryOp>(instr);
        if (unaryOp->expr()->opcode() == ohmu::til::COP_Literal) count++;
      } break;
      case ohmu::til::COP_BinaryOp: {
        auto binaryOp = ohmu::cast<ohmu::til::BinaryOp>(instr);
        if (binaryOp->expr0()->opcode() == ohmu::til::COP_Literal) count++;
        if (binaryOp->expr1()->opcode() == ohmu::til::COP_Literal) count++;
      } break;
      default:
        error("Unknown instruction type while counting literals.");
    }
  auto instr = basicBlock.terminator();
  switch (instr->opcode()) {
    case ohmu::til::COP_Goto:
      break;
    case ohmu::til::COP_Branch: {
      auto branch = ohmu::cast<ohmu::til::Branch>(instr);
      if (branch->condition()->opcode() == ohmu::til::COP_Literal) count++;
    } break;
    case ohmu::til::COP_Return: {
      auto ret = ohmu::cast<ohmu::til::Return>(instr);
      if (ret->returnValue()->opcode() == ohmu::til::COP_Literal) count++;
    } break;
    default:
      error("Unknown terminator type while counting literals.");
  }
  return count;
}

void ModuleBuilder::countLiterals() {
  for (auto& sidecar : blockSidecarArray)
    sidecar.literals.bound = countBlockLiterals(*sidecar.basicBlock);
  blockSidecarArray[0].literals.first = 0;
  for (auto& sidecar : blockSidecarArray.slice(1, -1))
    sidecar.literals.bound += sidecar.literals.first =
        (&sidecar)[-1].literals.bound;
  //for (auto& sidecar : blockSidecarArray)
  //  printf("count = [%d, %d)\n", sidecar.literals.first, sidecar.literals.bound);
}

//==============================================================================
// Building literalsArray
//==============================================================================

ohmu::til::Literal** buildBlockLiteralsArray(
    ohmu::til::BasicBlock& basicBlock,
    ohmu::til::Literal** literal) {
  for (auto instr : basicBlock.instructions()) switch (instr->opcode()) {
      case ohmu::til::COP_Load: { /*TODO: figure out address literals*/
      } break;
      case ohmu::til::COP_UnaryOp: {
        auto unaryOp = ohmu::cast<ohmu::til::UnaryOp>(instr);
        if (unaryOp->expr()->opcode() == ohmu::til::COP_Literal)
          *literal++ = ohmu::cast<ohmu::til::Literal>(unaryOp->expr());
      } break;
      case ohmu::til::COP_BinaryOp: {
        auto binaryOp = ohmu::cast<ohmu::til::BinaryOp>(instr);
        if (binaryOp->expr0()->opcode() == ohmu::til::COP_Literal)
          *literal++ = ohmu::cast<ohmu::til::Literal>(binaryOp->expr0());
        if (binaryOp->expr1()->opcode() == ohmu::til::COP_Literal)
          *literal++ = ohmu::cast<ohmu::til::Literal>(binaryOp->expr1());
      } break;
      default:
        error("Unknown instruction type while building literals.");
    }
  auto instr = basicBlock.terminator();
  switch (instr->opcode()) {
    case ohmu::til::COP_Goto:
      break;
    case ohmu::til::COP_Branch: {
      auto branch = ohmu::cast<ohmu::til::Branch>(instr);
      if (branch->condition()->opcode() == ohmu::til::COP_Literal)
        *literal++ = ohmu::cast<ohmu::til::Literal>(branch->condition());
    } break;
    case ohmu::til::COP_Return: {
      auto ret = ohmu::cast<ohmu::til::Return>(instr);
      if (ret->returnValue()->opcode() == ohmu::til::COP_Literal)
        *literal++ = ohmu::cast<ohmu::til::Literal>(ret->returnValue());
    } break;
    default:
      error("Unknown terminator type while building literals.");
  }
  return literal;
}

void ModuleBuilder::buildLiteralsArray() {
  if (!blockSidecarArray.last().literals.bound)
    return;
  literals =
      Array<ohmu::til::Literal*>(blockSidecarArray.last().literals.bound);
  auto p = literals.begin();
  for (auto& sidecar : blockSidecarArray)
    p = buildBlockLiteralsArray(*sidecar.basicBlock, p);
  assert(p == this->literals.end() && "We didn't find them all.");
  std::sort(literals.begin(), literals.end());
  size_t uniqueSize = 1;
  for (size_t i = 1, e = literals.size(); i != e; ++i)
    if (literals[i] != literals[i - 1])
      uniqueSize++;
  Array<ohmu::til::Literal*> swap(uniqueSize);
  swap[0] = literals[0];
  for (size_t i = 1, j = 1, e = literals.size(); i != e; ++i)
    if (literals[i] != literals[i - 1])
      swap[j++] = literals[i];
  literals = move(swap);
  module.constDataEntries = Array<wax::StaticData>(literals.size());
  auto literalEntries = module.constDataEntries.begin();
  uint i = 0;
  for (auto literal : literals) {
    uint size;
    switch (literal->baseType().Size) {
      case ohmu::til::BaseType::ST_8: size = 1; break;
      case ohmu::til::BaseType::ST_16: size = 2; break;
      case ohmu::til::BaseType::ST_32: size = 4; break;
      case ohmu::til::BaseType::ST_64: size = 8; break;
      default: error("Unsupported literal size.");
    }
    literalEntries[i++].alignment = literalEntries[i].bytes.bound = size;
  }
  literalEntries->bytes.first = 0;
  for (auto& entry : module.constDataEntries.slice(1, -1))
    entry.bytes.bound += entry.bytes.first =
        (&entry)[-1].bytes.bound + (entry.alignment - 1) &
        ~(entry.alignment - 1);
  module.constData = Array<char>(module.constDataEntries.last().bytes.bound);
  auto data = module.constData.begin();
  i = 0;
  for (auto literal : literals) {
    auto addr = data + literalEntries[i].bytes.first;
    literal->setStackID(i++);
    switch (literal->baseType().Size) {
      case ohmu::til::BaseType::ST_8:
        *(uchar*)addr = literal->as<uchar>().value();
      case ohmu::til::BaseType::ST_16:
        *(ushort*)addr = literal->as<ushort>().value();
        break;
      case ohmu::til::BaseType::ST_32:
        *(uint*)addr = literal->as<uint>().value();
        //printf("%02x - %02x : %d\n", dataEntry[-1].bytes.first,
        //       dataEntry[-1].bytes.bound, literal->as<uint>().value());
        break;
      case ohmu::til::BaseType::ST_64:
        *(uint64*)addr = literal->as<uint64>().value();
        break;
    }
  }
}

//==============================================================================
// Event counting
//==============================================================================

void countBlockEvents(wax::Block& block,
                      const ohmu::til::BasicBlock& basicBlock) {
  size_t count = 0;
  if (block.dominator != INVALID_INDEX) count += wax::BlockHeader::SLOT_COUNT;
  count += block.predecessors.size() * wax::Phi::SLOT_COUNT;
  for (auto instr : basicBlock.instructions()) switch (instr->opcode()) {
      //case ohmu::til::COP_Load: {
      //  count += wax::Load::SLOT_COUNT;
      //} break;
      //case ohmu::til::COP_Store: {
      //  auto store = ohmu::cast<ohmu::til::Store>(instr);
      //  if (store->source()->opcode() == ohmu::til::COP_Literal)
      //    count += wax::Load::SLOT_COUNT;
      //  count += wax::Store::SLOT_COUNT;
      //} break;
      case ohmu::til::COP_UnaryOp: {
        auto unaryOp = ohmu::cast<ohmu::til::UnaryOp>(instr);
        if (unaryOp->expr()->opcode() == ohmu::til::COP_Literal)
          count += wax::StaticAddress::SLOT_COUNT + wax::Load::SLOT_COUNT;
        count += wax::local::Unary<wax::NOP, uint>::SLOT_COUNT;
      } break;
      case ohmu::til::COP_BinaryOp: {
        auto binaryOp = ohmu::cast<ohmu::til::BinaryOp>(instr);
        if (binaryOp->expr0()->opcode() == ohmu::til::COP_Literal)
          count += wax::StaticAddress::SLOT_COUNT + wax::Load::SLOT_COUNT;
        if (binaryOp->expr1()->opcode() == ohmu::til::COP_Literal)
          count += wax::StaticAddress::SLOT_COUNT + wax::Load::SLOT_COUNT;
        count += wax::local::Binary<wax::NOP, uint>::SLOT_COUNT;
      } break;
      default:
        error("Unknown instruction type while building literals.");
    }
  auto instr = basicBlock.terminator();
  switch (instr->opcode()) {
    case ohmu::til::COP_Goto:
      count += wax::Jump::SLOT_COUNT;
      break;
    case ohmu::til::COP_Branch: {
      auto branch = ohmu::cast<ohmu::til::Branch>(instr);
      if (branch->condition()->opcode() == ohmu::til::COP_Literal)
        count += wax::StaticAddress::SLOT_COUNT + wax::Load::SLOT_COUNT;
      count += wax::Branch::SLOT_COUNT;
    } break;
    case ohmu::til::COP_Return: {
      auto ret = ohmu::cast<ohmu::til::Return>(instr);
      if (ret->returnValue()->opcode() == ohmu::til::COP_Literal)
        count += wax::StaticAddress::SLOT_COUNT + wax::Load::SLOT_COUNT;
      count += wax::Return::SLOT_COUNT + 1; // 1 for the argument
    } break;
    default:
      error("Unknown terminator type while building literals.");
  }
  if (block.phiIndex == INVALID_INDEX) count += wax::Return::SLOT_COUNT;
  block.events.bound = count;
}

void ModuleBuilder::countEvents() {
  auto blocks = module.blockArray.begin();
  auto sidecar = blockSidecarArray.begin();
  for (size_t i = 0, e = module.blockArray.size(); i != e; ++i)
    countBlockEvents(blocks[i], *sidecar[i].basicBlock);
  blocks[0].events.first = 0;
  for (size_t i = 1, e = module.blockArray.size(); i != e; ++i)
    blocks[i].events.bound += blocks[i].events.first = blocks[i - 1].events.bound;
  module.instrArray.init(module.blockArray.last().events.bound);
}

wax::Type translateType(const ohmu::til::BaseType& type) {
  wax::Type::Kind kind;
  switch (type.Base) {
    case ohmu::til::BaseType::BT_Void:
      kind = wax::Type::VOID;
      assert(type.Size == ohmu::til::BaseType::ST_0);
      break;
    case ohmu::til::BaseType::BT_Bool:
      kind = wax::Type::BOOLEAN;
      assert(type.Size == ohmu::til::BaseType::ST_1);
      break;
    case ohmu::til::BaseType::BT_Int:
      kind = wax::Type::INTEGER;
      break;
    case ohmu::til::BaseType::BT_UnsignedInt:
      kind = wax::Type::UNSIGNED;
      break;
    case ohmu::til::BaseType::BT_Float:
      kind = wax::Type::FLOAT;
      break;
    case ohmu::til::BaseType::BT_String:
      kind = wax::Type::ADDRESS;
      break;
    case ohmu::til::BaseType::BT_Pointer:
      kind = wax::Type::ADDRESS;
      break;
  }
  wax::Type::Size size;
  switch (type.Size) {
    case ohmu::til::BaseType::ST_0:
      size = wax::Type::BYTE;
      assert(type.Base == ohmu::til::BaseType::BT_Void);
      break;
    case ohmu::til::BaseType::ST_1:
      size = wax::Type::BYTE;
      assert(type.Base == ohmu::til::BaseType::BT_Bool);
      break;
    case ohmu::til::BaseType::ST_8:
      size = wax::Type::BYTE;
      break;
    case ohmu::til::BaseType::ST_16:
      size = wax::Type::SHORT;
      break;
    case ohmu::til::BaseType::ST_32:
      size = wax::Type::WORD;
      break;
    case ohmu::til::BaseType::ST_64:
      size = wax::Type::LONG;
      break;
    case ohmu::til::BaseType::ST_128:
      size = wax::Type::BYTE;
      error("Back-end doesn't support 128-bit types.");
      break;
  }
  wax::Type::Count count;
  switch (type.VectSize) {
    case 0:
    case 1:
      count = wax::Type::SCALAR;
      break;
    case 2:
      count = wax::Type::VEC2;
      break;
    case 3:
      count = wax::Type::VEC3;
      break;
    case 4:
      count = wax::Type::VEC4;
      break;
    default:
      error("Unsupported vector size.");
  }
  return wax::Type(kind, size, count);
}

TypedRef emitImmediateLoad(TypedRef event, const ohmu::til::Literal* literal) {
  auto staticAddress = event.index();
  event = event.as<wax::StaticAddress>().init(
      wax::Label(wax::Label::CONSTANT, literal->stackID()));
  event = event.as<wax::Load>().init(
      wax::LoadStorePayload(translateType(literal->baseType())), staticAddress);
  return event;
}

void buildBlockEvents(wax::Block* blocks, TypedPtr events, wax::Block& block,
                      const ModuleBuilder::BlockSidecar& sidecar) {
  TypedRef event = events[block.events.first];
  if (block.dominator != INVALID_INDEX)
    event = event.as<wax::BlockHeader>().init(blocks, block);
  for (size_t j = 0, e = block.predecessors.size(); j != e; ++j)
    event = event.as<wax::Phi>().init();
  for (auto instr : sidecar.basicBlock->instructions())
    switch (instr->opcode()) {
      //case ohmu::til::COP_Load: {
      //  error("unsupported");
      //  auto load = ohmu::cast<ohmu::til::Load>(instr);
      //  //load->pointer
      //  events.type(i + 0) = wax::LOAD;
      //  events.type(i + 1) = wax::USE; // TODO: or static address.
      //  events.data(i + 0) = 0; // TODO: LoadStorePayload
      //  events.data(i + 1) = 0; // TODO: either address or label 
      //} break;
      //case ohmu::til::COP_Store: {
      //  error("unsupported");
      //  auto store = ohmu::cast<ohmu::til::Store>(instr);
      //  if (store->source()->opcode() == ohmu::til::COP_Literal)
      //    count += wax::Load::SLOT_COUNT;
      //  count += wax::Store::SLOT_COUNT;
      //} break;
      case ohmu::til::COP_UnaryOp: {
        auto unaryOp = ohmu::cast<ohmu::til::UnaryOp>(instr);
        uint arg;
        wax::Type type;
        if (unaryOp->expr()->opcode() == ohmu::til::COP_Literal) {
          auto literal = ohmu::cast<ohmu::til::Literal>(unaryOp->expr());
          event = emitImmediateLoad(event, literal);
          arg = event.index() - wax::Load::SLOT_COUNT;
          type = translateType(literal->baseType());
        } else {
          auto instr = ohmu::cast<ohmu::til::Instruction>(unaryOp->expr());
          arg = instr->stackID();
          type = translateType(instr->baseType());
        }
        auto payload = wax::TypedPayload(translateType(unaryOp->baseType()));
        switch (unaryOp->unaryOpcode()) {
          case ohmu::til::UOP_BitNot:
            event = event.as<wax::Not>().init(payload, arg);
            break;
          case ohmu::til::UOP_LogicNot:
            event = event.as<wax::Not>().init(payload, arg);
            break;
          case ohmu::til::UOP_Minus:
            event = event.as<wax::Neg>().init(payload, arg);
            break;
          default:
            error("Unknown unary op.");
        }
      } break;
      case ohmu::til::COP_BinaryOp: {
        auto binaryOp = ohmu::cast<ohmu::til::BinaryOp>(instr);
        uint arg0, arg1;
        wax::Type type;
        if (binaryOp->expr0()->opcode() == ohmu::til::COP_Literal) {
          auto literal = ohmu::cast<ohmu::til::Literal>(binaryOp->expr0());
          event = emitImmediateLoad(event, literal);
          arg0 = event.index() - wax::Load::SLOT_COUNT;
          type = translateType(literal->baseType());
        } else {
          auto instr = ohmu::cast<ohmu::til::Instruction>(binaryOp->expr0());
          arg0 = instr->stackID();
          type = translateType(instr->baseType());
        }
        if (binaryOp->expr1()->opcode() == ohmu::til::COP_Literal) {
          auto literal = ohmu::cast<ohmu::til::Literal>(binaryOp->expr1());
          event = emitImmediateLoad(event, literal);
          arg1 = event.index() - wax::Load::SLOT_COUNT;
        } else {
          auto instr = ohmu::cast<ohmu::til::Instruction>(binaryOp->expr1());
          arg1 = instr->stackID();
        }
        auto payload = wax::TypedPayload(type);
        switch (binaryOp->binaryOpcode()) {
        case ohmu::til::BOP_Add:
          event = event.as<wax::Add>().init(payload, arg0, arg1);
          break;
        case ohmu::til::BOP_Sub:
          event = event.as<wax::Sub>().init(payload, arg0, arg1);
          break;
        case ohmu::til::BOP_Mul:
          event = event.as<wax::Mul>().init(payload, arg0, arg1);
          break;
        case ohmu::til::BOP_Div:
          event = event.as<wax::Div>().init(payload, arg0, arg1);
          break;
        case ohmu::til::BOP_Rem:
          event = event.as<wax::Mod>().init(payload, arg0, arg1);
          break;
        case ohmu::til::BOP_Shl:
          event = event.as<wax::Shift>().init(
              wax::ShiftPayload(payload.type, wax::ShiftPayload::LEFT), arg0,
              arg1);
          break;
        case ohmu::til::BOP_Shr:
          event = event.as<wax::Shift>().init(
            wax::ShiftPayload(payload.type, wax::ShiftPayload::RIGHT), arg0,
            arg1);
          break;
        case ohmu::til::BOP_BitAnd:
          event = event.as<wax::Logic>().init(
              wax::LogicPayload(payload.type, wax::LogicPayload::AND), arg0,
              arg1);
          break;
        case ohmu::til::BOP_BitXor:
          event = event.as<wax::Logic>().init(
            wax::LogicPayload(payload.type, wax::LogicPayload::XOR), arg0,
            arg1);
          break;
        case ohmu::til::BOP_BitOr:
          event = event.as<wax::Logic>().init(
            wax::LogicPayload(payload.type, wax::LogicPayload::OR), arg0,
            arg1);
          break;
        case ohmu::til::BOP_Eq:
          event = event.as<wax::Compare>().init(
              wax::ComparePayload(type, wax::ComparePayload::EQ), arg0, arg1);
          break;
        case ohmu::til::BOP_Neq:
          event = event.as<wax::Compare>().init(
            wax::ComparePayload(type, wax::ComparePayload::NEQ), arg0, arg1);
          break;
        case ohmu::til::BOP_Lt:
          event = event.as<wax::Compare>().init(
            wax::ComparePayload(type, wax::ComparePayload::LT), arg0, arg1);
          break;
        case ohmu::til::BOP_Leq:
          event = event.as<wax::Compare>().init(
            wax::ComparePayload(type, wax::ComparePayload::LE), arg0, arg1);
          break;
        default:
          error("Unknown binary op.");
        }
      } break;
      default:
        error("Unknown instruction type while building literals.");
    }
  auto instr = sidecar.basicBlock->terminator();
  switch (instr->opcode()) {
    case ohmu::til::COP_Goto:
      event = event.as<wax::Jump>().init(
          sidecar.entryBlockID +
          ohmu::cast<ohmu::til::Goto>(instr)->targetBlock()->blockID());
      break;
    case ohmu::til::COP_Branch: {
      auto branch = ohmu::cast<ohmu::til::Branch>(instr);
      uint arg;
      if (branch->condition()->opcode() == ohmu::til::COP_Literal) {
        assert(false && "This shouldn't happen.");
        auto literal = ohmu::cast<ohmu::til::Literal>(branch->condition());
        event = emitImmediateLoad(event, literal);
        arg = event.index() - wax::Load::SLOT_COUNT;
      }
      else {
        auto instr = ohmu::cast<ohmu::til::Instruction>(branch->condition());
        arg = instr->stackID();
      }
      event = event.as<wax::Branch>().init(arg,
          sidecar.entryBlockID + branch->thenBlock()->blockID(),
          sidecar.entryBlockID + branch->elseBlock()->blockID());
    } break;
    case ohmu::til::COP_Return: {
      auto ret= ohmu::cast<ohmu::til::Return>(instr);
      uint arg;
      if (ret->returnValue()->opcode() == ohmu::til::COP_Literal) {
        assert(false && "This shouldn't happen.");
        auto literal = ohmu::cast<ohmu::til::Literal>(ret->returnValue());
        event = emitImmediateLoad(event, literal);
        arg = event.index() - wax::Load::SLOT_COUNT;
      }
      else {
        auto instr = ohmu::cast<ohmu::til::Instruction>(ret->returnValue());
        arg = instr->stackID();
      }
      event = event.as<wax::Use>().init(arg);
      event = event.as<wax::Return>().init(1);
    } break;
    default:
      error("Unknown terminator type while building literals.");
  }
}

void ModuleBuilder::buildEventsArray() {
  auto blocks = module.blockArray.begin();
  auto sidecar = blockSidecarArray.begin();
  auto events = module.instrArray.root;
  for (size_t i = 0, e = module.blockArray.size(); i != e; ++i)
    buildBlockEvents(blocks, events, blocks[i], sidecar[i]);
  //void buildBlockEvents(wax::Block* blocks, TypedPtr events, wax::Block& block,
  //  const ModuleBuilder::BlockSidecar& sidecar,
  //  const ohmu::til::BasicBlock& basicBlock) {
  }
}  // namespace {

//==============================================================================
// Externally Visible Functions
//==============================================================================

void buildModuleFromTIL(wax::Module& module, ohmu::til::Global& global) {
  ModuleBuilder builder(module, global);
  builder.walkTILGraph();
  builder.buildFunctionArray();
  builder.buildBlockSidecarArray();
  builder.buildBlockArray();
  builder.countLiterals();
  builder.buildLiteralsArray();
  builder.countEvents();
  //builder.buildEventArray();
}
}  // namespace jagger

#if 0
namespace Jagger {
//extern void printDebug(EventBuilder builder, size_t numEvents);
//extern void normalize(const EventList& in);

struct Block {
  static const size_t NO_DOMINATOR = (size_t)-1;
  ohmu::til::BasicBlock* basicBlock;
  Block* list;
  size_t dominator;
  size_t head;
  size_t firstEvent;
  size_t boundEvent;
  size_t phiSlot;
};

uchar typeDesc(const ohmu::til::ValueType& type) {
  static const uchar jaggerType[ohmu::til::ValueType::BT_ValueRef + 1] = {
      BINARY_DATA,    BINARY_DATA,    UNSIGNED_INTEGER, FLOAT,
      SIGNED_INTEGER, SIGNED_INTEGER, SIGNED_INTEGER};
  static const uchar logBits[ohmu::til::ValueType::ST_128 + 1] = {
      LOG1 << 2,  LOG1 << 2,  LOG8 << 2,  LOG16 << 2,
      LOG32 << 2, LOG64 << 2, LOG128 << 2};
  uchar x = jaggerType[type.Base];
  if (type.Signed && type.Base == ohmu::til::ValueType::BT_Int)
    x = SIGNED_INTEGER;
  x |= logBits[type.Size];
  if (type.VectSize) {
    assert(!(type.VectSize & type.VectSize - 1));
    unsigned long size = type.VectSize;
    unsigned long log;
    _BitScanReverse(&log, size);
    x |= log << 5;
  }
  return x;
}

size_t (*emitLiteralTable[32])(EventBuilder, size_t, ohmu::til::Literal&);
size_t (*emitUnaryOpIntTable[32])(EventBuilder, size_t, ohmu::til::UnaryOp&);
size_t (*emitBinaryOpIntTable[32])(EventBuilder, size_t, ohmu::til::BinaryOp&);
size_t (*emitInstructionTable[32])(EventBuilder, size_t, ohmu::til::Instruction&);
size_t (*emitTerminatorTable[32])(EventBuilder, size_t, Block&);

size_t emitBlockHeader(EventBuilder builder, size_t i, Block& block) {
  auto blocks = block.list;
  if (block.dominator == Block::NO_DOMINATOR)
    return builder.op(i, NOP, 0);
  if (blocks + block.head != &block)
    return builder.op(i, JOIN_HEADER, blocks[block.head].boundEvent);
  if (blocks[block.dominator].boundEvent == block.firstEvent)
    return builder.op(i, NOP, 0);
  return builder.op(i, CASE_HEADER, blocks[block.dominator].boundEvent);
}

size_t emitPhi(EventBuilder builder, size_t i, ohmu::til::Phi& phi) {
  return builder.op(i, PHI, 0);
}

// Expression emission
size_t emitIntLiteral(EventBuilder builder, size_t i,
                      ohmu::til::Literal& literal) {
  i = builder.op(i, IMMEDIATE_BYTES, (uint)literal.as<int>().value());
  return builder.op(i, ANCHOR, 0);
}

size_t emitInstruction(EventBuilder builder, size_t i,
                       ohmu::til::Instruction& instr) {
  return (emitInstructionTable[instr.opcode()])(builder, i, instr);
}

// TODO: handle vectors and sizes!
size_t emitLiteral(EventBuilder builder, size_t i,
                   ohmu::til::Instruction& instr) {
  auto& literal = *ohmu::cast<ohmu::til::Literal>(&instr);
  i = emitLiteralTable[literal.valueType().Base](builder, i, literal);
  literal.setStackID(i - 1);
  return i;
}

size_t emitUnaryOp(EventBuilder builder, size_t i,
                   ohmu::til::Instruction& instr) {
  auto& unaryOp = *ohmu::cast<ohmu::til::UnaryOp>(&instr);
  auto arg = ohmu::cast<ohmu::til::Instruction>(unaryOp.expr());
  if (arg->isTrivial()) i = emitInstruction(builder, i, *arg);
  return emitUnaryOpIntTable[unaryOp.unaryOpcode()](
      builder, unaryOp.setStackID(i), unaryOp);
}

size_t emitBinaryOp(EventBuilder builder, size_t i,
                    ohmu::til::Instruction& instr) {
  auto& binaryOp = *ohmu::cast<ohmu::til::BinaryOp>(&instr);
  auto arg0 = ohmu::cast<ohmu::til::Instruction>(binaryOp.expr0());
  auto arg1 = ohmu::cast<ohmu::til::Instruction>(binaryOp.expr1());
  if (arg0->isTrivial()) i = emitInstruction(builder, i, *arg0);
  if (arg1->isTrivial()) i = emitInstruction(builder, i, *arg1);
  //TODO: pull these out and initialize them dynamically
  static const uchar opcodeTable[] = {
    /* BOP_Add      =*/ ADD,
    /* BOP_Sub      =*/ SUB,
    /* BOP_Mul      =*/ MUL,
    /* BOP_Div      =*/ DIV,
    /* BOP_Rem      =*/ IMOD,
    /* BOP_Shl      =*/ NOP,
    /* BOP_Shr      =*/ NOP,
    /* BOP_BitAnd   =*/ LOGIC,
    /* BOP_BitXor   =*/ LOGIC,
    /* BOP_BitOr    =*/ LOGIC,
    /* BOP_Eq       =*/ COMPARE,
    /* BOP_Neq      =*/ COMPARE,
    /* BOP_Lt       =*/ COMPARE,
    /* BOP_Leq      =*/ COMPARE,
    /* BOP_Gt       =*/ COMPARE,
    /* BOP_Geq      =*/ COMPARE,
    /* BOP_LogicAnd =*/ LOGIC,
    /* BOP_LogicOr  =*/ LOGIC,
  };
  static const uchar controlTable[] = {
    /* BOP_Add      =*/ 0,
    /* BOP_Sub      =*/ 0,
    /* BOP_Mul      =*/ 0,
    /* BOP_Div      =*/ 0,
    /* BOP_Rem      =*/ 0,
    /* BOP_Shl      =*/ 0,
    /* BOP_Shr      =*/ 0,
    /* BOP_BitAnd   =*/ LOGICAL_AND,
    /* BOP_BitXor   =*/ LOGICAL_XOR,
    /* BOP_BitOr    =*/ LOGICAL_OR,
    /* BOP_Eq       =*/ CMP_EQ,
    /* BOP_Neq      =*/ CMP_NEQ,
    /* BOP_Lt       =*/ CMP_LT,
    /* BOP_Leq      =*/ CMP_LE,
    /* BOP_Gt       =*/ CMP_GT,
    /* BOP_Geq      =*/ CMP_GE,
    /* BOP_LogicAnd =*/ LOGICAL_AND,
    /* BOP_LogicOr  =*/ LOGICAL_OR,
  };
  uchar code = opcodeTable[binaryOp.binaryOpcode()];
  i = builder.op(i, LAST_USE, arg0->stackID());
  i = builder.op(i, LAST_USE, arg1->stackID());
  i = builder.op(
      i, code, code == COMPARE
                   ? (unsigned)CompareData(
                         typeDesc(arg0->valueType()),
                         (Compare)controlTable[binaryOp.binaryOpcode()])
                   : code == LOGIC
                         ? (unsigned)LogicData(
                               typeDesc(binaryOp.valueType()),
                               (Logic)controlTable[binaryOp.binaryOpcode()])
                         : (unsigned)BasicData(typeDesc(binaryOp.valueType())));
  binaryOp.setStackID(i);
  return builder.op(i, ANCHOR, 0);
}

size_t emitTerminator(EventBuilder builder, size_t i, Block& block) {
  return emitTerminatorTable[block.basicBlock->terminator()->opcode()](
      builder, i, block);
}

size_t emitGoto(EventBuilder builder, size_t i, Block& block) {
  auto& targetBasicBlock = *ohmu::cast<ohmu::til::Goto>(
                                block.basicBlock->terminator())->targetBlock();
  auto& targetBlock = block.list[targetBasicBlock.blockID()];
  auto phiSlot = block.phiSlot;
  auto phiOffset = targetBlock.firstEvent + 1;
  for (auto arg : targetBasicBlock.arguments()) {
    auto target =
        builder.empty() ? 0 : ohmu::cast<ohmu::til::Instruction>(
                                  arg->values()[phiSlot].get())->stackID();
    i = builder.op(i, LAST_USE, target);
    i = builder.op(i, JOIN_COPY, phiOffset++);
  }
  return builder.op(i, JUMP, targetBlock.firstEvent);
}

size_t emitBranch(EventBuilder builder, size_t i, Block& block) {
  auto& branch = *ohmu::cast<ohmu::til::Branch>(block.basicBlock->terminator());
  auto arg = ohmu::cast<ohmu::til::Instruction>(branch.condition());
  if (arg->isTrivial()) i = emitInstruction(builder, i, *arg);
  auto& thenBlock = block.list[branch.thenBlock()->blockID()];
  auto& elseBlock = block.list[branch.elseBlock()->blockID()];
  i = builder.op(i, LAST_USE, arg->stackID());
  i = builder.op(i, BRANCH, elseBlock.firstEvent);
  i = builder.op(i, BRANCH_TARGET, thenBlock.firstEvent);
  return i;
}

size_t emitReturn(EventBuilder builder, size_t i, Block& block) {
  auto& ret = *ohmu::cast<ohmu::til::Return>(block.basicBlock->terminator());
  auto arg = ohmu::cast<ohmu::til::Instruction>(ret.returnValue());
  if (arg->isTrivial()) i = emitInstruction(builder, i, *arg);
  return builder.op(i, RET, arg->stackID());
}

void initTables() {
  // Always instantiate to avoid race conditions.
  //emitBinaryOpIntTable[ohmu::til::BOP_Add] = emitBinaryOpIntAdd;
  //emitBinaryOpIntTable[ohmu::til::BOP_Sub] = emitBinaryOpIntSub;
  //emitBinaryOpIntTable[ohmu::til::BOP_Mul] = emitBinaryOpIntMul;
  //emitBinaryOpIntTable[ohmu::til::BOP_Eq] = emitBinaryOpIntEq;
  //emitBinaryOpIntTable[ohmu::til::BOP_Lt] = emitBinaryOpIntLt;
  //emitBinaryOpIntTable[ohmu::til::BOP_Leq] = emitBinaryOpIntLeq;

  emitInstructionTable[ohmu::til::COP_Literal] = emitLiteral;
  emitInstructionTable[ohmu::til::COP_UnaryOp] = emitUnaryOp;
  emitInstructionTable[ohmu::til::COP_BinaryOp] = emitBinaryOp;

  emitLiteralTable[ohmu::til::ValueType::BT_Int] = emitIntLiteral;

  emitTerminatorTable[ohmu::til::COP_Goto] = emitGoto;
  emitTerminatorTable[ohmu::til::COP_Branch] = emitBranch;
  emitTerminatorTable[ohmu::til::COP_Return] = emitReturn;
}

size_t initBlock(Block* blocks, ohmu::til::BasicBlock& basicBlock) {
  auto blockID = basicBlock.blockID();
  auto& block = blocks[basicBlock.blockID()];
  block.list = blocks;
  block.basicBlock = &basicBlock;
  block.head = blockID;
  block.dominator = Block::NO_DOMINATOR;

  // Assign phi slots
  if (basicBlock.arguments().size())
    for (size_t i = 0, e = basicBlock.predecessors().size(); i != e; ++i)
      blocks[basicBlock.predecessors()[i]->blockID()].phiSlot = i;

  // Assign events offsets.
  block.firstEvent = 0;
  EventBuilder builder(nullptr);
  size_t i = emitBlockHeader(builder, 0, block);
  for (auto arg : basicBlock.arguments()) i = emitPhi(builder, i, *arg);
  for (auto instr : basicBlock.instructions())
    i = emitInstruction(builder, i, *instr);
  i = emitTerminator(builder, i, block);
  return block.boundEvent = i;
}

void initBlockDominators(Block& block) {
  auto& basicBlock = *block.basicBlock;
  if (ohmu::til::BasicBlock* parent = basicBlock.parent()) {
    block.dominator = parent->blockID();
    //if (basicBlock.postDominates(*parent) || block.dominator + 1 == block.head)
    if (basicBlock.postDominates(*parent))
      block.head = block.list[block.dominator].head;
  }
}

size_t emitBlock(EventBuilder builder, size_t i, Block& block) {
  auto& basicBlock = *block.basicBlock;
  i = emitBlockHeader(builder, block.firstEvent, block);
  for (auto arg : basicBlock.arguments())
    i = emitPhi(builder, arg->setStackID(i), *arg);
  for (auto instr : basicBlock.instructions())
    i = emitInstruction(builder, i, *instr);
  return emitTerminator(builder, i, block);
}

void emitEvents(ohmu::til::Global& global) {
  initTables();

  // Visit all of the CFGs
  ohmu::til::VisitCFG visitCFG;
  visitCFG.traverseAll(global.global());
  auto& cfgs = visitCFG.cfgs();
  auto numCFGs = cfgs.size();
  if (!numCFGs) return;

  // Generate offsets for each CFG in the block array.
  auto cfgOffsets = new size_t[numCFGs + 1];
  *cfgOffsets = 0;
  for (size_t i = 0; i < numCFGs; i++)
    cfgOffsets[i + 1] = cfgOffsets[i] + cfgs[i]->numBlocks();
  auto numBlocks = cfgOffsets[numCFGs];

  // Allocate and initialize the 
  auto blocks = new Block[numBlocks];

  // Initialize the blocks (both loops are parallel safe)
  size_t numEvents = 0;
  for (size_t i = 0; i < numCFGs; i++)
    for (auto& basicBlock : cfgs[i]->blocks())
      numEvents += initBlock(blocks + cfgOffsets[i], *basicBlock.get());

  // Check to see if we're going to run out of address space.
  if ((uint)numEvents != numEvents) {
    printf("Too many events!\n");
    delete[] cfgOffsets;
    delete[] blocks;
    return;
  }

  // Initialize the block dominators (outer loop is parallel safe)
  for (size_t i = 0; i < numCFGs; i++)
    for (auto& basicBlock : cfgs[i]->blocks())
      initBlockDominators(blocks[cfgOffsets[i] + basicBlock->blockID()]);

  // Perform block prefix sum of block events.
  EventList eventList;
  eventList.init(numEvents);
  size_t i = eventList.first;
  for (auto& block : AdaptRange(blocks, numBlocks)) {
    block.firstEvent = i;
    i = block.boundEvent += i;
    printf("block %d : %d-%d\n", block.basicBlock->blockID(), block.firstEvent,
           block.boundEvent);
  }

  // Emit the events.
  i = eventList.first;
  for (auto& block : AdaptRange(blocks, numBlocks))
    i = emitBlock(eventList.builder, i, block);

  normalize(eventList);
  printDebug(eventList.builder, numEvents);
  //delete[] eventBuffer;

  printf("numCFGs = %d\n", numCFGs);
  printf("numBlocks = %d\n", numBlocks);

  // Clean up.
  delete[] cfgOffsets;
  delete[] blocks;
}

} // namespace Jagger

#if 0
int buffer[1000];
for (size_t size = 1; size < 16; size++) {
  size_t o = (size + 2) / 3;
  auto p = (char*)buffer - o / 4 * 4;
  for (size_t i = 0; i < 1000; i++)
    buffer[i] = 0;
  for (size_t i = 0; i < size; i++) {
    p[i + o] = i + 1;
    ((unsigned*)p)[i + o] = (i + 1) * 0x1010101;
  }
  auto bound = ((o * 3 + 3) / 4 + size) * 4;
  //printf("%d : %d : %d\n", size, o, bound);
  if (bound > 80)
    bound = 80;
  for (size_t i = 0; i < bound; i++)
    printf("%x", 0xf & ((unsigned char*)buffer)[i]);
  if (bound != 80)
    printf("\n");
}
#endif
#endif