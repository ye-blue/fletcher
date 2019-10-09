// Copyright 2018 Delft University of Technology
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fletchgen/nucleus.h"

#include <cerata/api.h>
#include <utility>
#include <vector>
#include <string>

#include "fletchgen/basic_types.h"
#include "fletchgen/recordbatch.h"
#include "fletchgen/kernel.h"
#include "fletchgen/mmio.h"
#include "fletchgen/profiler.h"
#include "fletchgen/axi4_lite.h"

namespace fletchgen {

ArrayCmdCtrlMerger::ArrayCmdCtrlMerger() : Component("ArrayCmdCtrlMerger") {
  // This is a primitive component from the hardware lib
  meta_[cerata::vhdl::metakeys::PRIMITIVE] = "true";
  meta_[cerata::vhdl::metakeys::LIBRARY] = "work";
  meta_[cerata::vhdl::metakeys::PACKAGE] = "Array_pkg";

  auto reg64 = cerata::Vector::Make(64);
  auto baw = Parameter::Make("bus_addr_width", integer(), intl(64));
  auto idw = Parameter::Make("index_width", integer(), intl(32));
  auto tw = Parameter::Make("tag_width", integer(), intl(1));
  auto cw = Parameter::Make("num_addr", integer(), intl(0));
  auto nucleus_side_cmd = Port::Make("nucleus_cmd", cmd(tw, cw), Port::Dir::OUT, kernel_cd());
  auto kernel_side_cmd = Port::Make("kernel_cmd", cmd(tw), Port::Dir::IN, kernel_cd());
  auto ctrl = PortArray::Make("ctrl", reg64, cw, Port::Dir::IN, kernel_cd());
  Add({baw, idw, tw, nucleus_side_cmd, kernel_side_cmd, ctrl});
}

std::unique_ptr<Instance> ArrayCmdCtrlMergerInstance(const std::string &name) {
  std::unique_ptr<Instance> result;
  // Check if the Array component was already created.
  Component *merger_comp;
  auto optional_component = cerata::default_component_pool()->Get("ArrayCmdCtrlMerger");
  if (optional_component) {
    merger_comp = *optional_component;
  } else {
    auto merger_comp_shared = std::make_shared<ArrayCmdCtrlMerger>();
    cerata::default_component_pool()->Add(merger_comp_shared);
    merger_comp = merger_comp_shared.get();
  }
  // Create and return an instance of the Array component.
  result = Instance::Make(merger_comp, name);
  return result;
}

static void CopyFieldPorts(Component *nucleus,
                           const RecordBatch &record_batch,
                           FieldPort::Function fun) {
  // Add Arrow field derived ports with some function.
  auto field_ports = record_batch.GetFieldPorts(fun);
  for (const auto &fp : field_ports) {
    // Create a copy and invert for the Nucleus
    auto copied_port = std::dynamic_pointer_cast<FieldPort>(fp->Copy());
    copied_port->InvertDirection();
    nucleus->Add(copied_port);
  }
}

Nucleus::Nucleus(const std::string &name,
                 const std::vector<std::shared_ptr<RecordBatch>> &recordbatches,
                 const std::shared_ptr<Kernel> &kernel,
                 const std::shared_ptr<Component> &mmio)
    : Component(name) {

  // Add address width parameter.
  Add(bus_addr_width());
  // Add clock/reset
  auto kcd = Port::Make("kcd", cr(), Port::Dir::IN, kernel_cd());
  Add(kcd);
  // Add AXI4-lite interface
  auto axi = axi4_lite(Port::Dir::IN);
  Add(axi);

  // Instantiate the kernel and connect the clock/reset.
  kernel_inst = AddInstanceOf(kernel.get());
  Connect(kernel_inst->port("kcd"), kcd.get());

  // Instantiate the MMIO component and connect the AXI4-lite port and clock/reset.
  auto mmio_inst = AddInstanceOf(mmio.get());
  mmio_inst->port("mmio") <<= axi;
  mmio_inst->port("kcd") <<= kcd;
  // For the kernel user, we need to abstract the "ctrl" field of the command streams away.
  // We need to instantiate a little ArrayCommandCtrlMerger (accm) component that just adds the buffer addresses to
  // the cmd stream ctrl field. We will remember the instances of that component and we'll get the buffer address ports
  // for later on.
  std::vector<Instance *> accms;
  // Get all the buffer ports from the mmio instance.
  std::vector<MmioPort*> mmio_buffer_ports;
  for (const auto& p : mmio_inst->GetAll<MmioPort>()) {
    if (p->reg.function == MmioReg::Function::BUFFER) {
      mmio_buffer_ports.push_back(p);
    }
  }

  // Copy over the ports from the RecordBatches.
  for (const auto &rb : recordbatches) {
    CopyFieldPorts(this, *rb, FieldPort::Function::ARROW);
    CopyFieldPorts(this, *rb, FieldPort::Function::UNLOCK);

    // For each one of the command streams, make an inverted copy of the RecordBatch unabstracted command stream port.
    // This one will expose all command stream fields to the nucleus user.
    for (const auto &cmd : rb->GetFieldPorts(FieldPort::Function::COMMAND)) {
      auto nucleus_cmd = std::dynamic_pointer_cast<FieldPort>(cmd->Copy());
      nucleus_cmd->InvertDirection();
      Add(nucleus_cmd);
      // Now, instantiate an ACCM that will merge the buffer addresses onto the command stream at the nucleus level.
      auto accm_inst = ArrayCmdCtrlMergerInstance(cmd->name() + "_accm_inst");
      // Remember the instance.
      accms.push_back(accm_inst.get());
      // Move ownership to this Nucleus component.
      AddChild(std::move(accm_inst));
    }
  }

  // Add and connect all recordbatch ports
  size_t batch_idx = 0;
  size_t accm_idx = 0;
  size_t buf_idx = 0;
  for (const auto &r : recordbatches) {
    // Connect Arrow data stream
    for (const auto &ap : r->GetFieldPorts(FieldPort::Function::ARROW)) {
      auto kernel_data = kernel_inst->port(ap->name());
      auto nucleus_data = port(ap->name());
      std::shared_ptr<cerata::Edge> edge;
      if (ap->dir() == Port::OUT) {
        edge = Connect(kernel_data, nucleus_data);
      } else {
        edge = Connect(nucleus_data, kernel_data);
      }
    }

    // Connect unlock stream
    for (const auto &up : r->GetFieldPorts(FieldPort::Function::UNLOCK)) {
      auto kernel_unl = kernel_inst->port(up->name());
      auto nucleus_unl = port(up->name());
      Connect(kernel_unl, nucleus_unl);
    }

    // Connect the command stream through the ACCM.
    size_t field_idx = 0;
    for (const auto &cmd : r->GetFieldPorts(FieldPort::Function::COMMAND)) {
      // Get the ports on either side of the ACCM.
      auto accm_nucleus_cmd = accms[accm_idx]->port("nucleus_cmd");
      auto accm_kernel_cmd = accms[accm_idx]->port("kernel_cmd");
      auto accm_ctrl = accms[accm_idx]->porta("ctrl");
      // Get the corresponding cmd ports on this nucleus and the kernel.
      auto nucleus_cmd = port(cmd->name());
      auto kernel_cmd = kernel_inst->port(cmd->name());

      // Connect the nucleus cmd to the accm cmd and the accm command to the kernel cmd.
      Connect(nucleus_cmd, accm_nucleus_cmd);
      Connect(accm_kernel_cmd, kernel_cmd);

      // To connect the buffer addresses from the mmio to the accm, we need to figure out which buffers there are.
      // We can look this up in the RecordBatchDescription.
      auto field_bufs = r->batch_desc().fields[field_idx].buffers;
      for (size_t b = 0; b < field_bufs.size(); b++) {
        Connect(accm_ctrl->Append(), mmio_buffer_ports[buf_idx]);
        buf_idx++;
      }
      field_idx++;
      accm_idx++;
    }
    batch_idx++;
  }

  // Perform some magic to abstract the buffer addresses away from the ctrl stream at the kernel level.
  // First, obtain the intended name for the kernel from the metadata of the vhdmmio component port metadata.
  // Then, make a connection between these two components.
  for (auto &p : mmio_inst->GetAll<MmioPort>()) {
    if (ExposeToKernel(p->reg.function)) {
      auto inst_port = kernel_inst->port(p->reg.name);
      if (p->dir() == Port::Dir::OUT) {
        Connect(inst_port, p);
      } else {
        Connect(p, inst_port);
      }
    }
  }
}

std::shared_ptr<Nucleus> nucleus(const std::string &name,
                                 const std::vector<std::shared_ptr<RecordBatch>> &recordbatches,
                                 const std::shared_ptr<Kernel> &kernel,
                                 const std::shared_ptr<Component> &mmio) {
  return std::make_shared<Nucleus>(name, recordbatches, kernel, mmio);
}

std::vector<FieldPort *> Nucleus::GetFieldPorts(FieldPort::Function fun) const {
  std::vector<FieldPort *> result;
  for (const auto &ofp : GetNodes()) {
    auto fp = dynamic_cast<FieldPort *>(ofp);
    if (fp != nullptr) {
      if (fp->function_ == fun) {
        result.push_back(fp);
      }
    }
  }
  return result;
}

}  // namespace fletchgen
