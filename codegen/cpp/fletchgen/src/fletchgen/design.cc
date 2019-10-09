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

#include <cerata/api.h>

#include <string>
#include <deque>
#include <regex>
#include <algorithm>

#include "fletcher/common.h"
#include "fletchgen/design.h"
#include "fletchgen/recordbatch.h"
#include "fletchgen/mmio.h"
#include "fletchgen/profiler.h"

namespace fletchgen {

using cerata::OutputSpec;
using F = MmioReg::Function;
using B = MmioReg::Behavior;

/// Short-hand for vector of RecordBatches.
using RBVector = std::vector<std::shared_ptr<arrow::RecordBatch>>;

static std::optional<std::shared_ptr<arrow::RecordBatch>> GetRecordBatchWithName(const RBVector &batches,
                                                                                 const std::string &name) {
  for (const auto &b :  batches) {
    if (fletcher::GetMeta(*b->schema(), "fletcher_name") == name) {
      return b;
    }
  }
  return std::nullopt;
}

void Design::AnalyzeSchemas() {
  // Attempt to create a SchemaSet from all schemas that can be detected in the options.
  FLETCHER_LOG(INFO, "Creating SchemaSet.");
  schema_set = SchemaSet::Make(options->kernel_name);
  // Add all schemas from the list of schema files
  for (const auto &arrow_schema : options->schemas) {
    schema_set->AppendSchema(arrow_schema);
  }
  // Add all schemas from the recordbatches and add all recordbatches.
  for (const auto &recordbatch : options->recordbatches) {
    schema_set->AppendSchema(recordbatch->schema());
  }
  // Sort the schema set according to the recordbatch ordering specification.
  // Important for the control flow through MMIO / buffer addresses.
  // First we sort recordbatches by name, then by mode.
  schema_set->Sort();
}

void Design::AnalyzeRecordBatches() {
  // Now that we have every Schema, for every Schema, figure out if there is a RecordBatch in the input options.
  // If there is, add a description of the RecordBatch to this design.
  // If there isn't, create a virtual RecordBatch based on the schema.
  for (const auto &fletcher_schema : schema_set->schemas()) {
    auto rb = GetRecordBatchWithName(options->recordbatches, fletcher_schema->name());
    fletcher::RecordBatchDescription rbd;
    if (rb) {
      fletcher::RecordBatchAnalyzer rba(&rbd);
      rba.Analyze(**rb);
    } else {
      fletcher::SchemaAnalyzer sa(&rbd);
      sa.Analyze(*fletcher_schema->arrow_schema());
    }
    batch_desc.push_back(rbd);
  }
}

static std::vector<MmioReg> GetDefaultRegs() {
  std::vector<MmioReg> result;
  result.push_back(MmioReg{F::DEFAULT, B::CONTROL, "start", "Start the kernel.", 1, 0, 0});
  result.push_back(MmioReg{F::DEFAULT, B::CONTROL, "stop", "Stop the kernel.", 1, 0, 1});
  result.push_back(MmioReg{F::DEFAULT, B::CONTROL, "reset", "Reset the kernel.", 1, 0, 2});
  result.push_back(MmioReg{F::DEFAULT, B::STATUS, "idle", "Kernel idle status.", 1, 4, 0});
  result.push_back(MmioReg{F::DEFAULT, B::STATUS, "busy", "Kernel busy status.", 1, 4, 1});
  result.push_back(MmioReg{F::DEFAULT, B::STATUS, "done", "Kernel done status.", 1, 4, 2});
  result.push_back(MmioReg{F::DEFAULT, B::STATUS, "result", "Result.", 64, 8, 0});
  return result;
}

static std::vector<MmioReg> ParseCustomRegs(const std::vector<std::string> &regs) {
  std::vector<MmioReg> result;
  // Iterate and parse every string.
  for (const auto &reg_str : regs) {
    std::regex expr(R"([c|s][\:][\d]+[\:][\w]+)");
    if (std::regex_match(reg_str, expr)) {
      MmioReg reg;
      reg.function = MmioReg::Function::KERNEL;
      auto w_start = reg_str.find(':') + 1;
      auto i_start = reg_str.find(':', w_start) + 1;
      auto width_str = reg_str.substr(w_start, i_start - w_start);
      reg.name = reg_str.substr(i_start);
      reg.width = static_cast<uint32_t>(std::strtoul(width_str.c_str(), nullptr, 10));
      switch (reg_str[0]) {
        case 'c':reg.behavior = MmioReg::Behavior::CONTROL;
          break;
        case 's':reg.behavior = MmioReg::Behavior::STATUS;
          break;
        default:FLETCHER_LOG(FATAL, "Register argument behavior character invalid for " + reg.name);
      }
      // Mark the register as a custom register for the kernel.
      reg.meta["kernel"] = "true";
      result.push_back(reg);
    }
  }
  return result;
}

/// @brief Generate mmio registers from properly ordered RecordBatchDescriptions.
static std::vector<MmioReg> GetRecordBatchRegs(const std::vector<fletcher::RecordBatchDescription> &batch_desc) {
  std::vector<MmioReg> result;

  // Get first and last indices.
  for (const auto &r : batch_desc) {
    result.push_back(MmioReg({F::BATCH, B::CONTROL, r.name + "_firstidx", r.name + " first index.", 32}));
    result.push_back(MmioReg({F::BATCH, B::CONTROL, r.name + "_lastidx", r.name + " last index (exclusive).", 32}));
  }

  // Get all buffer addresses.
  for (const auto &r : batch_desc) {
    for (const auto &f : r.fields) {
      for (const auto &b : f.buffers) {
        auto buffer_port_name = r.name + "_" + fletcher::ToString(b.desc_);
        // buf_port_names->push_back(buffer_port_name);
        result.push_back(MmioReg({F::BUFFER, B::CONTROL, buffer_port_name,
                                  "Buffer address for " + r.name + " " + fletcher::ToString(b.desc_), 64}));
      }
    }
  }
  return result;
}

Design::Design(const std::shared_ptr<Options> &opts) {
  options = opts;

  // Analyze schemas and recordbatches to get schema_set and batch_desc
  AnalyzeSchemas();
  AnalyzeRecordBatches();
  // Sanity check our design for equal number of schemas and recordbatch descriptions.
  if (schema_set->schemas().size() != batch_desc.size()) {
    FLETCHER_LOG(FATAL, "Number of Schemas and RecordBatchDescriptions does not match.");
  }

  // Now that we have parsed some of the options, generate the design from the bottom up.
  // The order in which to do this is from components that sink/source the kernel, to the kernel, and then to the
  // upper layers of the hierarchy.

  // Generate a RecordBatchReader/Writer component for every FletcherSchema / RecordBatchDesc.
  for (size_t i = 0; i < batch_desc.size(); i++) {
    auto schema = schema_set->schemas()[i];
    auto rb_desc = batch_desc[i];
    auto rb = recordbatch(schema, rb_desc);
    recordbatches.push_back(rb);
  }

  // Generate the MMIO component model for this. This is based on four things;
  // 1. The default registers (like control, status, result).
  // 2. The RecordBatchDescriptions - for every recordbatch we need a first and last index, and every buffer address.
  // 3. The custom kernel registers, parsed from the command line arguments.
  // 4. The profiling registers, obtained from inspecting the generated recordbatches.
  default_regs = GetDefaultRegs();
  recordbatch_regs = GetRecordBatchRegs(batch_desc);
  kernel_regs = ParseCustomRegs(opts->regs);
  profiling_regs = GetProfilingRegs(recordbatches);

  // Merge these registers together into one register file for component generation.
  std::vector<MmioReg> regs;
  regs.insert(regs.end(), default_regs.begin(), default_regs.end());
  regs.insert(regs.end(), recordbatch_regs.begin(), recordbatch_regs.end());
  regs.insert(regs.end(), kernel_regs.begin(), kernel_regs.end());
  regs.insert(regs.end(), profiling_regs.begin(), profiling_regs.end());

  // Generate a Yaml file for vhdmmio based on the recordbatch description
  auto ofs = std::ofstream("fletchgen.mmio.yaml");
  ofs << GenerateVhdmmioYaml(&regs);
  ofs.close();

  // Generate the MMIO component.
  mmio_comp = mmio(batch_desc, regs);
  // Generate the kernel.
  kernel_comp = kernel(opts->kernel_name, recordbatches, mmio_comp);
  // Generate the nucleus.
  nucleus_comp = nucleus(opts->kernel_name + "_Nucleus", recordbatches, kernel_comp, mmio_comp);
  // Generate the mantle.
  mantle_comp = mantle(opts->kernel_name + "_Mantle", recordbatches, nucleus_comp);

  // Run vhdmmio
  auto vhdmmio_result = system("vhdmmio -V vhdl -H -P vhdl");
  if (vhdmmio_result != 0) {
    FLETCHER_LOG(FATAL, "vhdmmio exited with status " << vhdmmio_result);
  }
}

std::deque<cerata::OutputSpec> Design::GetOutputSpec() {
  std::deque<OutputSpec> result;
  OutputSpec omantle, okernel, onucleus;

  std::string backup = options->backup ? "true" : "false";
  // Mantle
  omantle.comp = mantle_comp;
  // Always overwrite mantle, as users should not modify.
  omantle.meta[cerata::vhdl::metakeys::BACKUP_EXISTING] = backup;
  result.push_back(omantle);

  // Nucleus
  onucleus.comp = nucleus_comp;
  // Check the force flag if kernel should be overwritten
  onucleus.meta[cerata::vhdl::metakeys::BACKUP_EXISTING] = backup;
  result.push_back(onucleus);

  // Kernel
  okernel.comp = kernel_comp;
  // Check the force flag if kernel should be overwritten
  okernel.meta[cerata::vhdl::metakeys::BACKUP_EXISTING] = backup;
  result.push_back(okernel);

  // RecordBatchReaders/Writers
  for (const auto &recbatch : recordbatches) {
    OutputSpec orecbatch;
    orecbatch.comp = recbatch;
    // Always overwrite readers/writers, as users should not modify.
    orecbatch.meta[cerata::vhdl::metakeys::BACKUP_EXISTING] = backup;
    result.push_back(orecbatch);
  }

  return result;
}

}  // namespace fletchgen
