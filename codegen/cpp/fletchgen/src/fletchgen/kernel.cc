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

#include "fletchgen/kernel.h"

#include <cerata/api.h>
#include <utility>
#include <string>
#include <vector>

#include "fletchgen/basic_types.h"
#include "fletchgen/mmio.h"

namespace fletchgen {

static void CopyFieldPorts(Component *kernel,
                           const RecordBatch &record_batch,
                           FieldPort::Function fun) {
  // Add Arrow field derived ports with some function.
  auto field_ports = record_batch.GetFieldPorts(fun);
  for (const auto &fp : field_ports) {
    // Create a copy and invert for the Kernel
    auto copied_port = std::dynamic_pointer_cast<FieldPort>(fp->Copy());
    copied_port->InvertDirection();
    kernel->Add(copied_port->Copy());
  }
}

Kernel::Kernel(std::string name,
               const std::vector<std::shared_ptr<RecordBatch>> &recordbatches,
               const std::shared_ptr<Component> &mmio)
    : Component(std::move(name)) {

  // Add clock/reset
  Add(Port::Make("kcd", cr(), Port::Dir::IN, kernel_cd()));

  // Add ports going to/from RecordBatches.
  for (const auto &r : recordbatches) {
    // Copy over the Arrow data and unlock stream ports.
    CopyFieldPorts(this, *r, FieldPort::Function::ARROW);
    CopyFieldPorts(this, *r, FieldPort::Function::UNLOCK);

    // The command stream at the kernel interface enjoys some abstraction, namely; the buffer addresses in the ctrl
    // field are abstracted away from the kernel user. We create new command ports based on the command ports of the
    // RecordBatch, but leave out the ctrl field.
    auto rb_cmds = r->GetFieldPorts(FieldPort::Function::COMMAND);
    for (auto &rb_cmd : rb_cmds) {
      // Next, make an abstracted version of the command stream for the kernel user.
      auto kernel_cmd = FieldPort::MakeCommandPort(rb_cmd->fletcher_schema_, rb_cmd->field_, false, kernel_cd());
      kernel_cmd->InvertDirection();
      Add(kernel_cmd);
    }
  }

  // Add ports from mmio
  for (const auto &p : mmio->GetAll<Port>()) {
    // Only copy over mmio ports that have the kernel function.
    auto mmio_port = dynamic_cast<MmioPort *>(p);
    if (mmio_port != nullptr) {
      if (ExposeToKernel(mmio_port->reg.function)) {
        auto kernel_port = std::dynamic_pointer_cast<MmioPort>(p->Copy());
        kernel_port->InvertDirection();
        kernel_port->SetName(mmio_port->reg.name);
        Add(kernel_port);
      }
    }
  }

}

std::shared_ptr<Kernel> kernel(const std::string &name,
                               const std::vector<std::shared_ptr<RecordBatch>> &recordbatches,
                               const std::shared_ptr<Component> &mmio) {
  return std::make_shared<Kernel>(name, recordbatches, mmio);
}

}  // namespace fletchgen
