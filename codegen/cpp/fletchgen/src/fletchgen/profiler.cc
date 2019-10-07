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

#include <memory>

#include "fletchgen/basic_types.h"

namespace fletchgen {

using cerata::Component;
using cerata::Parameter;
using cerata::Port;
using cerata::Stream;
using cerata::Vector;
using cerata::integer;
using cerata::bit;
using cerata::nul;

std::shared_ptr<cerata::Type> stream_probe() {
  auto result = Stream::Make("", nul());
  return result;
}

static std::shared_ptr<Component> Profiler() {
  // Parameters
  auto out_count_max = Parameter::Make("OUT_COUNT_MAX", integer(), cerata::intl(1023));
  auto out_count_width = Parameter::Make("OUT_COUNT_WIDTH", integer(), cerata::intl(10));

  auto pcr = Port::Make("pcd", cr(), Port::Dir::IN);
  auto probe = Port::Make("probe", stream_probe(), Port::Dir::IN);
  auto enable = Port::Make("enable", bit(), Port::Dir::IN);
  auto count = Port::Make("count", Vector::Make("out_count_type", out_count_width), Port::Dir::OUT);

  // Component & ports
  auto ret = Component::Make("StreamProfiler", {out_count_max, out_count_width, pcr, probe, enable, count});

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
  for (auto n : comp->GetNodes()) {
    // Flatten the types
    auto fts = Flatten(n->type());
    int s = 0;
    for (size_t f = 0; f < fts.size(); f++) {
      if (fts[f].type_->Is(Type::STREAM)) {
        if (n->meta.count(PROFILE) > 0) {
          if (n->meta.at(PROFILE) == "true") {
            FLETCHER_LOG(INFO, "Inserting profiler for stream node " + n->name()
                + ", sub-stream " + std::to_string(s)
                + " of flattened type " + n->type()->name()
                + " index " + std::to_string(f) + ".");
            auto domain = GetDomain(*n);
            if (!domain) {
              throw std::runtime_error("No clock domain specified for stream node ["
                                           + n->name() + "] on component ["
                                           + comp->name() + ".");
            }
            auto cr_node = GetClockResetPort(comp, **domain);
            if (!cr_node) {
              throw std::runtime_error("No clock/reset port present on component [" + comp->name()
                                           + "] for clock domain [" + (*domain)->name()
                                           + "] of stream node [" + n->name() + "].");
            }

            // Instantiate a profiler
            std::string name = fts[f].name(cerata::NamePart(n->name(), true));

            auto profiler_inst_unique = ProfilerInstance(name + "_StreamProfiler_inst", *domain);
            auto profiler_inst = profiler_inst_unique.get();
            comp->AddChild(std::move(profiler_inst_unique));

            // Obtain the profiler ports
            auto p_probe = profiler_inst->port("probe");
            auto p_count = profiler_inst->port("count");
            auto p_en = profiler_inst->port("enable");

            // Copy profiler data and control to the top level port.
            auto c_count = std::dynamic_pointer_cast<Node>(p_count->Copy());
            auto c_en = std::dynamic_pointer_cast<Node>(p_en->Copy());
            c_count->SetName(name + "_count");
            c_en->SetName(name + "_enable");
            comp->Add(c_count);
            comp->Add(c_en);

            // Set up a type mapper.
            auto mapper = TypeMapper::Make(n->type(), p_probe->type());
            auto matrix = mapper->map_matrix().Empty();
            matrix(f, 0) = 1;
            mapper->SetMappingMatrix(matrix);
            n->type()->AddMapper(mapper);

            // Connect the probe, clock/reset, count and enable
            Connect(p_probe, n);
            Connect(profiler_inst->port("pcd"), *cr_node);
            c_count <<= p_count;
            p_en <<= c_en;

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
