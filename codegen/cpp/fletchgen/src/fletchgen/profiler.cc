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

#include "fletchgen/profiler.h"

#include <cerata/api.h>
#include <cmath>
#include <memory>
#include <vector>

#include "fletchgen/basic_types.h"
#include "fletchgen/nucleus.h"

namespace fletchgen {

using cerata::Component;
using cerata::Parameter;
using cerata::Port;
using cerata::Stream;
using cerata::Vector;
using cerata::integer;
using cerata::bit;
using cerata::nul;

// Vhdmmio documentation strings for profiling:
static constexpr char ecount[] = "Element count. Accumulates the number of elements transferred on the stream. "
                                 "Writing to the register subtracts the written value.";
static constexpr char vcount[] = "Valid count. Increments each cycle that the stream is valid. "
                                 "Writing to the register subtracts the written value.";
static constexpr char rcount[] = "Ready count. Increments each cycle that the stream is ready. "
                                 "Writing to the register subtracts the written value.";
static constexpr char tcount[] = "Transfer count. "
                                 "Increments for each transfer on the stream, i.e. when it is handshaked. "
                                 "Writing to the register subtracts the written value.";
static constexpr char pcount[] = "Packet count. Increments each time the last signal is set during a handshake "
                                 "Writing to the register subtracts the written value.";

std::vector<MmioReg> GetProfilingRegs(const std::vector<std::shared_ptr<RecordBatch>> &recordbatches) {
  std::vector<MmioReg> profile_regs;
  for (const auto &rb : recordbatches) {
    auto fps = rb->GetFieldPorts();
    for (const auto &fp : fps) {
      // Check if we should profile the field-derived port node.
      if (fp->profile_) {
        for (const auto &ft : cerata::Flatten(fp->type())) {
          if (ft.type_->Is(cerata::Type::STREAM)) {
            auto prefix = ft.name(cerata::NamePart(fp->name()));
            MmioReg e{MmioReg::Function::PROFILE, MmioReg::Behavior::STATUS, prefix + "_ecount", ecount, 32};
            MmioReg v{MmioReg::Function::PROFILE, MmioReg::Behavior::STATUS, prefix + "_vcount", vcount, 32};
            MmioReg r{MmioReg::Function::PROFILE, MmioReg::Behavior::STATUS, prefix + "_rcount", rcount, 32};
            MmioReg t{MmioReg::Function::PROFILE, MmioReg::Behavior::STATUS, prefix + "_tcount", tcount, 32};
            MmioReg p{MmioReg::Function::PROFILE, MmioReg::Behavior::STATUS, prefix + "_pcount", pcount, 32};
            profile_regs.insert(profile_regs.end(), {e, v, r, t, p});
          }
        }
      }
    }
  }
  return profile_regs;
}

std::shared_ptr<cerata::Type> stream_probe() {
  auto result = Stream::Make("", nul());
  return result;
}

static std::shared_ptr<Component> Profiler() {
  // Parameters
  auto out_count_max = Parameter::Make("OUT_COUNT_MAX", integer(), cerata::intl(1023));
  auto out_count_width = Parameter::Make("OUT_COUNT_WIDTH", integer(), cerata::intl(10));
  auto out_count_type = Vector::Make("out_count_type", out_count_width);

  auto pcr = Port::Make("pcd", cr(), Port::Dir::IN);
  auto probe = Port::Make("probe", stream_probe(), Port::Dir::IN);
  auto enable = Port::Make("enable", bit(), Port::Dir::IN);
  auto ecount = Port::Make("ecount", out_count_type, Port::Dir::OUT);
  auto vcount = Port::Make("vcount", out_count_type, Port::Dir::OUT);
  auto rcount = Port::Make("rcount", out_count_type, Port::Dir::OUT);
  auto tcount = Port::Make("tcount", out_count_type, Port::Dir::OUT);
  auto pcount = Port::Make("pcount", out_count_type, Port::Dir::OUT);

  // Component & ports
  auto ret = Component::Make("StreamProfiler", {out_count_max, out_count_width,
                                                pcr,
                                                probe,
                                                enable,
                                                ecount, vcount, rcount, tcount, pcount});

  // VHDL metadata
  ret->SetMeta(cerata::vhdl::metakeys::PRIMITIVE, "true");
  ret->SetMeta(cerata::vhdl::metakeys::LIBRARY, "work");
  ret->SetMeta(cerata::vhdl::metakeys::PACKAGE, "Stream_pkg");

  return ret;
}

std::unique_ptr<cerata::Instance> ProfilerInstance(const std::string &name,
                                                   const std::shared_ptr<ClockDomain> &domain) {
  std::unique_ptr<cerata::Instance> result;
  // Check if the Profiler component was already created.
  Component *profiler_component;
  auto optional_component = cerata::default_component_pool()->Get("StreamProfiler");
  if (optional_component) {
    profiler_component = *optional_component;
  } else {
    profiler_component = Profiler().get();
  }
  // Create and return an instance of the Array component.
  result = cerata::Instance::Make(profiler_component, name);
  result->port("pcd")->SetDomain(domain);
  // Because we can have multiple probes mapping to multiple stream types, each probe type should be unique.
  auto probe = result->port("probe");
  probe->SetType(stream_probe());
  probe->SetDomain(domain);
  return result;
}

static void AttachStreamProfilers(cerata::Component *comp) {
  // Get all nodes and check if their type contains a stream, then check if they should be profiled.
  for (auto node : comp->GetNodes()) {
    if (node->meta.count(PROFILE) > 0) {
      if (node->meta.at(PROFILE) == "true") {
        // Flatten the types
        auto fts = Flatten(node->type());
        int s = 0;
        for (size_t f = 0; f < fts.size(); f++) {
          if (fts[f].type_->Is(Type::STREAM)) {
            FLETCHER_LOG(INFO, "Inserting profiler for stream node " + node->name()
                + ", sub-stream " + std::to_string(s)
                + " of flattened type " + node->type()->name()
                + " index " + std::to_string(f) + ".");
            auto domain = GetDomain(*node);
            if (!domain) {
              throw std::runtime_error("No clock domain specified for stream node ["
                                           + node->name() + "] on component ["
                                           + comp->name() + ".");
            }
            auto cr_node = GetClockResetPort(comp, **domain);
            if (!cr_node) {
              throw std::runtime_error("No clock/reset port present on component [" + comp->name()
                                           + "] for clock domain [" + (*domain)->name()
                                           + "] of stream node [" + node->name() + "].");
            }

            // Instantiate a profiler.
            std::string name = fts[f].name(cerata::NamePart(node->name(), true));

            auto profiler_inst_unique = ProfilerInstance(name + "_StreamProfiler_inst", *domain);
            auto profiler_inst = profiler_inst_unique.get();
            comp->AddChild(std::move(profiler_inst_unique));

            // Obtain the profiler probe port.
            auto p_probe = profiler_inst->port("probe");
            // Set up a type mapper.
            auto mapper = TypeMapper::Make(node->type(), p_probe->type());
            auto matrix = mapper->map_matrix().Empty();
            matrix(f, 0) = 1;
            mapper->SetMappingMatrix(matrix);
            node->type()->AddMapper(mapper);

            // Connect the probe, clock/reset, count and enable
            Connect(p_probe, node);
            Connect(profiler_inst->port("pcd"), *cr_node);

            // Increase the s-th stream index in the flattened type.
            s++;
          }
        }
      }
    }
  }
}

cerata::Component *EnableStreamProfiling(cerata::Component *top) {
  std::deque<cerata::Graph *> graphs;
  cerata::GetAllGraphs(top, &graphs, true);
  for (auto g : graphs) {
    if (g->IsComponent()) {
      auto c = dynamic_cast<Component *>(g);
      AttachStreamProfilers(c);
    }
  }
  return top;
}

}  // namespace fletchgen
