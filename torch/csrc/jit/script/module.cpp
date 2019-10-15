#include <torch/csrc/jit/script/module.h>
#include <c10/util/Exception.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/jit/export.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/operator.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/inliner.h>
#include <torch/csrc/jit/script/compiler.h>
#include <torch/csrc/jit/script/error_report.h>
#include <torch/csrc/jit/script/schema_matching.h>

namespace torch {
namespace jit {
namespace script {

static ModulePtr create_module_object(
    c10::QualifiedName class_name,
    std::shared_ptr<CompilationUnit> cu,
    bool shouldMangle = false) {
  // If the name is unqualified, prepend a `__torch__`, similar to what Python
  // does with `__main__` for top-level code.
  if (class_name.prefix().empty()) {
    class_name = c10::QualifiedName("__torch__", class_name.name());
  }
  if (shouldMangle && cu->get_class(class_name) != nullptr) {
    class_name = cu->mangle(class_name);
  }
  auto cls = ClassType::create(std::move(class_name), cu, /*is_module=*/true);
  cu->register_type(cls);
  return c10::ivalue::Object::create(
      c10::StrongTypePtr(std::move(cu), std::move(cls)), 0);
}

Module::Module(c10::QualifiedName class_name)
    : module_value_(create_module_object(
          std::move(class_name),
          std::make_shared<CompilationUnit>())) {}

Module::Module(
    std::shared_ptr<CompilationUnit> cu,
    const c10::ClassTypePtr& type)
    : module_value_(c10::ivalue::Object::create(
          c10::StrongTypePtr(std::move(cu), type),
          type->numAttributes())) {}

Module::Module(
    c10::QualifiedName class_name,
    std::shared_ptr<CompilationUnit> cu,
    bool shouldMangle)
    : module_value_(create_module_object(
          std::move(class_name),
          std::move(cu),
          shouldMangle)) {}

ModulePtr Module::module_object() const {
  if (!module_value_) {
    // User has created a Model without assigning it to something already
    // loaded. This is done in tests, and when using the .define method.
    module_value_ =
        create_module_object("Module", std::make_shared<CompilationUnit>());
  }
  return module_value_;
}

// first class mode runs models as first class objects,
// and does not force inlining everywhere. This is experimental
// as we bring up the system since it will degrade performance
// and may introduce bugs. test_jit.py provides context managers
// that enable it for specific tests.
thread_local bool inline_everything = true;
bool& getInlineEverythingMode() {
  return inline_everything;
}

void Module::to(at::Device device, at::ScalarType dtype, bool non_blocking) {
  to_impl(device, dtype, non_blocking);
}

void Module::to(at::ScalarType dtype, bool non_blocking) {
  to_impl(/*device=*/c10::nullopt, dtype, non_blocking);
}

void Module::to(at::Device device, bool non_blocking) {
  to_impl(device, /*dtype=*/c10::nullopt, non_blocking);
}

void Module::save(std::ostream& out, const ExtraFilesMap& extra_files) const {
#ifndef C10_MOBILE
  ExportModule(*this, out, extra_files, false);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void Module::save(const std::string& filename, const ExtraFilesMap& extra_files)
    const {
#ifndef C10_MOBILE
  ExportModule(*this, filename, extra_files, false);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void Module::_save_for_mobile(std::ostream& out, const ExtraFilesMap& extra_files) const {
#ifndef C10_MOBILE
  ExportModule(*this, out, extra_files, true);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void Module::_save_for_mobile(const std::string& filename, const ExtraFilesMap& extra_files)
    const {
#ifndef C10_MOBILE
  ExportModule(*this, filename, extra_files, true);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void module_state_to(
    const IValue& iv,
    const c10::optional<at::Device>& device,
    const c10::optional<at::ScalarType>& dtype,
    bool non_blocking) {
  // Need to access the `at::Tensor` as a `Variable` here.
  autograd::Variable variable = iv.toTensor();
  // Use the data's original device or dtype if not supplied here.
  auto new_data = variable.to(
      device.value_or(variable.device()),
      dtype.value_or(variable.scalar_type()),
      non_blocking);
  variable.set_data(new_data);
}

void Module::to_impl(
    const c10::optional<at::Device>& device,
    const c10::optional<at::ScalarType>& dtype,
    bool non_blocking) {
  // First call `to()` on every child module.
  for (auto p : get_modules()) {
    p.second.to_impl(device, dtype, non_blocking);
  }
  // Then convert every of our parameters.
  for (const auto& parameter : get_parameters()) {
    module_state_to(parameter.second, device, dtype, non_blocking);
  }
  // Then convert every tensor attributes (buffers).
  for (const auto& attr : get_attributes()) {
    if (attr.second.type()->isSubtypeOf(TensorType::get())) {
      module_state_to(attr.second, device, dtype, non_blocking);
    }
  }
}

using Slot = std::pair<c10::intrusive_ptr<c10::ivalue::Object>, size_t>;

// remove the first module argument, replacing any access of its
// parameters/attributes with extra_ivalue input Slots that hold what value to
// pass into the graph. Used for ONNX export to remove first-class modules
// so it can deal purely with parameters and inputs
std::pair<std::shared_ptr<Graph>, std::vector<Slot>> lower_graph(
    const ModulePtr& self,
    Graph& g_,
    size_t self_offset = 0) {
  std::shared_ptr<Graph> g = g_.copy();
  // Inline to remove method/function calls
  Inline(*g);

  std::vector<Slot> extra_ivalues;

  struct SlotHash {
    std::size_t operator()(const Slot& slot) const {
      auto obj_hash = std::hash<c10::ivalue::Object*>{}(slot.first.get());
      auto offset_hash = std::hash<size_t>{}(slot.second);
      return torch::hash_combine(obj_hash, offset_hash);
    }
  };
  std::unordered_map<Slot, size_t, SlotHash> slot_to_offset;
  struct ToScan {
    ModulePtr mod;
    Node* n;
    size_t offset;
  };
  std::vector<ToScan> to_scan;
  std::vector<Node*> to_clean; // nodes that should be dead at the end

  auto getOrAddSlot = [&](const Slot& slot) -> Value* {
    auto it = slot_to_offset.find(slot);
    if (it != slot_to_offset.end()) {
      size_t ivalues_start = g->inputs().size() - extra_ivalues.size();
      return g->inputs().at(ivalues_start + it->second);
    }
    extra_ivalues.emplace_back(slot);
    slot_to_offset[slot] = extra_ivalues.size() - 1;
    return g->addInput()->setType(slot.first->getSlot(slot.second).type());
  };

  auto self_value = g->inputs().at(self_offset);

  for (Use use : self_value->uses()) {
    to_scan.emplace_back(ToScan{self, use.user, use.offset});
  }
  while (to_scan.size() > 0) {
    auto e = to_scan.back();
    to_scan.pop_back();

    // when we lambda lift forks, first-class modules may be passed across
    // forks. This code recursively lowers the module in the fork call.
    if (e.n->kind() == prim::fork) {
      auto subgraph = e.n->g(attr::Subgraph);
      std::vector<Slot> new_slots;
      std::tie(subgraph, new_slots) = lower_graph(e.mod, *subgraph, e.offset);
      e.n->g_(attr::Subgraph, subgraph);
      for (const auto& slot : new_slots) {
        e.n->addInput(getOrAddSlot(slot));
      }
      e.n->removeInput(e.offset);
      continue;
    }
    if (e.n->kind() == prim::PythonOp) {
      throw ErrorReport(e.n->sourceRange()) << "Couldn't export Python method.";
    }
    if (e.n->kind() != prim::GetAttr) {
      throw ErrorReport(e.n->sourceRange())
          << "temporary: the only valid use of a module is looking up an "
             "attribute but found "
          << *e.n;
    }
    size_t slot_idx = e.mod->type()->getAttributeSlot(e.n->s(attr::name));
    auto iv = e.mod->getSlot(slot_idx);
    if (ClassTypePtr c = e.n->output()->type()->cast<ClassType>()) {
      if (c->is_module()) {
        for (Use use : e.n->output()->uses()) {
          to_scan.emplace_back(ToScan{iv.toObject(), use.user, use.offset});
        }
        to_clean.emplace_back(e.n);
        continue;
      }
    }
    e.n->output()->replaceAllUsesWith(
        getOrAddSlot(std::make_pair(e.mod, slot_idx)));
    e.n->destroy();
  }

  while (to_clean.size() > 0) {
    Node* n = to_clean.back();
    AT_ASSERT(!n->hasUses());
    n->destroy();
    to_clean.pop_back();
  }
  AT_ASSERT(!self_value->hasUses());
  g->eraseInput(self_offset);

  return std::make_pair(std::move(g), std::move(extra_ivalues));
}

static std::vector<at::Tensor> loadTensors(const std::vector<Slot>& slots) {
  std::vector<at::Tensor> result;
  result.reserve(slots.size());
  for (const auto& slot : slots) {
    result.emplace_back(slot.first->getSlot(slot.second).toTensor());
  }
  return result;
}

std::pair<std::shared_ptr<Graph>, std::vector<at::Tensor>> Method::
    _lowered_graph() {
  auto result = lower_graph(owner().module_object(), *graph());
  return std::make_pair(result.first, loadTensors(result.second));
}

Method::Method(ModulePtr owner, Function* function)
    : owner_(std::move(owner)), function_(function) {}

Module Method::owner() const {
  return Module(owner_);
}
void Method::run(Stack& stack) {
  stack.insert(stack.begin(), owner().module_object());
  function_->run(stack);
}

IValue Method::operator()(std::vector<IValue> stack, const Kwargs& kwargs) {
  stack.insert(stack.begin(), owner().module_object());
  return (*function_)(std::move(stack), kwargs);
}

void Module::define(const std::string& src, const ResolverPtr& resolver) {
  const auto self = SimpleSelf(type());
  class_compilation_unit()->define(
      name(), src, resolver ? resolver : script::nativeResolver(), &self);
}

void Module::clone_method(
    const Module& orig,
    const Function& method,
    const std::unordered_map<TypePtr, TypePtr>& type_remap) {
  // type remapping - when we copy method implementations from one module
  // singleton to another, we need to update the types of the self arguments
  // to match the new module.
  // XXX - this only handles modules that occur as variables, not modules
  // that appear in aggregate types. Currently this works fine because
  // we restrict how modules can be used during the lowering step. Eventually,
  // we will need to decide what it means for us to 'copy' a module.
  // For instance, we can copy just the state (parameters, attributes),
  // but share the code. Or we can copy the code. If we choose to copy the
  // code, what should we do about aggregate types that contain a module?
  auto type_remap_fn = [&](TypePtr in) {
    auto it = type_remap.find(in);
    if (it == type_remap.end())
      return in;
    return it->second;
  };
  auto graph = method.graph()->copy();
  graph->remapTypes(type_remap_fn);
  auto schema = method.getSchema().cloneWithRemappedTypes(type_remap_fn);
  const auto this_method_name = getNameForMethod(method.name());
  auto copied =
      class_compilation_unit()->create_function(this_method_name, graph);
  type()->addMethod(copied);
  copied->setSchema(std::move(schema));
}

void Module::clone_method(const Module& orig, const std::string& name) {
  std::unordered_map<TypePtr, TypePtr> type_remap;
  std::vector<std::pair<Module, Module>> to_scan = {{orig, *this}};
  while (!to_scan.empty()) {
    auto entry = to_scan.back();
    to_scan.pop_back();
    type_remap[entry.first.module_object()->type()] =
        entry.second.module_object()->type();
    for (auto p : entry.first.get_modules()) {
      to_scan.emplace_back(p.second, entry.second.get_module(p.first));
    }
  }
  return clone_method(orig, orig.get_method(name).function(), type_remap);
}

Module Module::clone() const {
  std::unordered_map<TypePtr, TypePtr> type_remap;
  return clone_impl(type_remap);
}

Module Module::clone_impl(
    std::unordered_map<TypePtr, TypePtr>& type_remap) const {
  // Create a new module_object in the same compilation unit.
  // The name is the same as for the original module, but it'll be mangled.
  // The class type is also created from scratch.
  Module r(name(), class_compilation_unit(), true);
  type_remap[type()] = r.type();

  // Copy slots. If a slot is a module - recursively clone it.
  for (auto p : get_slots()) {
    if (*entity_type(p.first) == EntityType::MODULE) {
      const Module& orig = Module(p.second.toObject());
      Module cloned = orig.clone_impl(type_remap);
      type_remap[orig.type()] = cloned.type();
      r.register_module(p.first, cloned);
    } else {
      r.register_attribute(
          p.first,
          p.second.type(),
          p.second,
          *entity_type(p.first) == EntityType::PARAMETER);
    }
  }

  // Clone methods remapping the types to the cloned ones.
  for (auto& fn : type()->methods()) {
    r.clone_method(*this, *fn, type_remap);
  }
  return r;
}

void Module::train(bool on) {
  for (auto p : get_modules()) {
    p.second.train(on);
  }
  if (auto slot = find_attribute("training")) {
    set_attribute("training", on);
  } else {
    TORCH_INTERNAL_ASSERT("'training' attribute not found");
  }
}

IValue Module::create_class(const c10::QualifiedName& name, Stack stack) const {
  // Look up the class
  const auto classType =
      class_compilation_unit()->get_class(c10::QualifiedName(name));
  if (!classType) {
    AT_ERROR(
        "Could not find class with name: '",
        name.qualifiedName(),
        "' in module.");
  }

  // Create a bare object with correct number of slots
  const size_t numAttrs = classType->numAttributes();
  auto obj = c10::ivalue::Object::create(
      c10::StrongTypePtr(class_compilation_unit(), classType), numAttrs);

  // Invoke the `__init__()` of the class with the arguments provided.
  Stack stackWithSelf = {obj};
  for (auto& arg : stack) {
    stackWithSelf.push_back(std::move(arg));
  }
  // Note: following Python, `__init__()` modifies its first parameter in-place
  // and returns nothing.
  classType->getMethod("__init__")->operator()(std::move(stackWithSelf));

  return obj;
}

ivalue_list Module::get_parameters() const {
  return ivalue_list(*this, EntityType::PARAMETER);
}

ivalue_list Module::get_attributes() const {
  return ivalue_list(*this, EntityType::ATTRIBUTE);
}

ivalue_list Module::get_slots() const {
  return ivalue_list(*this, c10::nullopt);
}

module_list Module::get_modules() const {
  return module_list(*this, EntityType::MODULE);
}

c10::optional<autograd::Variable> Module::find_parameter(
    const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && type()->is_parameter(*slot_idx)) {
    return autograd::as_variable_ref(
        module_object()->getSlot(*slot_idx).toTensor());
  }
  return c10::nullopt;
}

c10::optional<IValue> Module::find_attribute(const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && !type()->is_parameter(*slot_idx) &&
      !type()->is_module(*slot_idx)) {
    return module_object()->getSlot(*slot_idx);
  }
  return c10::nullopt;
}

c10::optional<autograd::Variable> Module::find_buffer(
    const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && !type()->is_parameter(*slot_idx) &&
      !type()->is_module(*slot_idx) &&
      type()->getAttribute(*slot_idx)->isSubtypeOf(TensorType::get())) {
    return autograd::as_variable_ref(
        module_object()->getSlot(*slot_idx).toTensor());
  }
  return c10::nullopt;
}

c10::optional<Module> Module::find_module(const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && type()->is_module(*slot_idx)) {
    return Module(module_object()->getAttr(name).toObject());
  }
  return c10::nullopt;
}

c10::optional<Method> Module::find_method(const std::string& basename) const {
  for (Function* fn : type()->methods()) {
    if (fn->name() == basename) {
      return Method(module_object(), fn);
    }
  }
  return c10::nullopt;
}

void Module::apply(const std::function<void(Module&)>& fn) {
  for (auto p : get_modules()) {
    p.second.apply(fn);
  }
  fn(*this);
}

std::string Module::dump_to_str(
    bool print_method_bodies,
    bool print_attr_values,
    bool print_param_values,
    int level = 0) const {
  std::stringstream ss;
  std::stringstream parameters_ss;
  std::stringstream attributes_ss;
  std::stringstream methods_ss;
  std::stringstream submodules_ss;

  for (auto p : get_parameters()) {
    parameters_ss << p.first << " = ";
    if (print_param_values) {
      parameters_ss << p.second.toTensor() << std::endl;
    } else {
      parameters_ss << "..." << std::endl;
    }
  }

  for (auto p : get_attributes()) {
    attributes_ss << p.first << " = ";
    if (!p.second.isTensor() || print_attr_values) {
      attributes_ss << p.second << std::endl;
    } else {
      attributes_ss << "..." << std::endl;
    }
  }

  for (const Method& method : get_methods()) {
    methods_ss << "  method " << method.name() << " {" << std::endl;
    if (print_method_bodies) {
      methods_ss << torch::jit::jit_log_prefix(
                        "    ", method.graph()->toString())
                 << std::endl;
    }
    methods_ss << "  }" << std::endl;
  }

  ss << "module " << name().qualifiedName() << " {" << std::endl;
  ss << "  parameters {" << std::endl;
  ss << torch::jit::jit_log_prefix("    ", parameters_ss.str());
  ss << "  }" << std::endl;
  ss << "  attributes {" << std::endl;
  ss << torch::jit::jit_log_prefix("    ", attributes_ss.str());
  ss << "  }" << std::endl;
  ss << "  methods {" << std::endl;
  ss << torch::jit::jit_log_prefix("  ", methods_ss.str());
  ss << "  }" << std::endl;
  ss << "  submodules {" << std::endl;
  for (const auto& p : get_modules()) {
    // We do level + 2, because one level of indentation comes from 'submodules'
    // scope and the other one goes from a specific submodule we're printing.
    ss << p.second.dump_to_str(
        print_method_bodies, print_attr_values, print_param_values, level + 2);
  }
  ss << "  }" << std::endl;
  ss << "}" << std::endl;

  std::string indent(2 * level, ' ');
  return torch::jit::jit_log_prefix(indent, ss.str());
}

void Module::dump(
    bool print_method_bodies = true,
    bool print_attr_values = true,
    bool print_param_values = true) const {
  std::cout << dump_to_str(
                   print_method_bodies,
                   print_attr_values,
                   print_param_values)
            << std::endl;
}

} // namespace script
} // namespace jit
} // namespace torch
