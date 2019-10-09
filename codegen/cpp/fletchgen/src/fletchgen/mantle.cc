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

#include "fletchgen/mantle.h"

#include <cerata/api.h>
#include <fletcher/common.h>

#include <memory>
#include <deque>
#include <utility>
#include <string>
#include <vector>

#include "fletchgen/basic_types.h"
#include "fletchgen/bus.h"
#include "fletchgen/nucleus.h"
#include "fletchgen/axi4_lite.h"

namespace fletchgen {

using cerata::intl;

static std::string ArbiterMasterName(BusSpec spec) {
  return std::string(spec.function == BusFunction::READ ? "rd" : "wr") + "_mst";
}

Mantle::Mantle(std::string name,
               const std::vector<std::shared_ptr<RecordBatch>> &recordbatches,
               const std::shared_ptr<Nucleus> &nucleus)
    : Component(std::move(name)) {

  // Add parameters.
  Add(bus_addr_width());

  // Add bus clock/reset, kernel clock/reset and AXI4-lite port.
  auto bcr = Port::Make("bcd", cr(), Port::Dir::IN, bus_cd());
  auto kcr = Port::Make("kcd", cr(), Port::Dir::IN, kernel_cd());
  auto regs = axi4_lite(Port::Dir::IN);
  Add({bcr, kcr, regs});

  // Instantiate the Nucleus and connect the ports.
  nucleus_inst_ = AddInstanceOf(nucleus.get());
  nucleus_inst_->port("kcd") <<= kcr;
  nucleus_inst_->port("mmio") <<= regs;

  // Instantiate all RecordBatches and connect the ports.
  for (const auto &rb : recordbatches) {
    auto rbi = AddInstanceOf(rb.get());
    recordbatch_instances_.push_back(rbi);

    // Connect ports.
    rbi->port("bcd") <<= bcr;
    rbi->port("kcd") <<= kcr;

    // Obtain all the field-derived ports from the RecordBatch Instance.
    auto field_ports = rbi->GetAll<FieldPort>();
    // Depending on the function and mode, connect each field port in the right way.
    for (const auto &fp : field_ports) {
      if (fp->function_ == FieldPort::Function::ARROW) {
        if (fp->dir() == cerata::Term::Dir::OUT) {
          Connect(nucleus_inst_->port(fp->name()), fp);
        } else {
          Connect(fp, nucleus_inst_->port(fp->name()));
        }
      } else if (fp->function_ == FieldPort::Function::COMMAND) {
        Connect(fp, nucleus_inst_->port(fp->name()));
      } else if (fp->function_ == FieldPort::Function::UNLOCK) {
        Connect(nucleus_inst_->port(fp->name()), fp);
      }
    }
  }

  std::deque<BusSpec> bus_specs;
  std::deque<BusPort *> bus_ports;

  // For all the bus interfaces, figure out which unique bus specifications there are.
  for (const auto &r : recordbatch_instances_) {
    auto r_bus_ports = r->GetAll<BusPort>();
    for (const auto &b : r_bus_ports) {
      bus_specs.push_back(b->spec_);
      bus_ports.push_back(b);
    }
  }

  // Leave only unique bus specs.
  auto last = std::unique(bus_specs.begin(), bus_specs.end());
  bus_specs.erase(last, bus_specs.end());

  // Generate a BusArbiterVec for every unique bus specification.
  for (const auto &spec : bus_specs) {
    FLETCHER_LOG(DEBUG, "Adding bus arbiter for: " + spec.ToString());
    auto arbiter_instance = BusArbiterInstance(spec);
    auto arbiter = arbiter_instance.get();
    AddChild(std::move(arbiter_instance));
    arbiter->par(bus_addr_width()->name()) <<= intl(spec.addr_width);
    arbiter->par(bus_data_width()->name()) <<= intl(spec.data_width);
    arbiter->par(bus_len_width()->name()) <<= intl(spec.len_width);
    if (spec.function == BusFunction::WRITE) {
      arbiter->par(bus_strobe_width()->name()) <<= intl(static_cast<int>(spec.data_width / 8));
    }
    arbiters_[spec] = arbiter;
    // Create the bus port on the mantle level.
    auto master = BusPort::Make(ArbiterMasterName(spec), Port::Dir::OUT, spec);
    Add(master);
    // TODO(johanpel): actually support multiple bus specs
    // Connect the arbiter master port to the mantle master port.
    master <<= arbiter->port("mst");
    // Connect the bus clock domain.
    arbiter->port("bcd") <<= bcr;
  }

  // Connect bus ports to the arbiters.
  for (const auto &bp : bus_ports) {
    // Get the arbiter port.
    auto arbiter = arbiters_.at(bp->spec_);
    auto arbiter_port_array = arbiter->porta("bsv");
    // Generate a mapper. TODO(johanpel): implement bus ports with same spec to map automatically.
    auto mapper = TypeMapper::MakeImplicit(bp->type(), arbiter_port_array->type());
    bp->type()->AddMapper(mapper);
    Connect(arbiter_port_array->Append(), bp);
  }
}

/// @brief Construct a Mantle and return a shared pointer to it.
std::shared_ptr<Mantle> mantle(const std::string &name,
                               const std::vector<std::shared_ptr<RecordBatch>> &recordbatches,
                               const std::shared_ptr<Nucleus> &nucleus) {
  return std::make_shared<Mantle>(name, recordbatches, nucleus);
}

}  // namespace fletchgen
