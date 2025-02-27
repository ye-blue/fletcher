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
#include <fletcher/common.h>

#include <fstream>

#include "fletchgen/options.h"
#include "fletchgen/design.h"
#include "fletchgen/utils.h"
#include "fletchgen/srec/recordbatch.h"
#include "fletchgen/top/sim.h"
#include "fletchgen/top/axi.h"
#include "fletchgen/fletchgen.h"

namespace fletchgen {

int fletchgen(int argc, char **argv) {
  // Start logging
  std::string program_name = fletchgen::GetProgramName(argv[0]);
  fletcher::StartLogging(program_name, FLETCHER_LOG_DEBUG, program_name + ".log");

  // Enable Cerata to log into the Fletcher logger through the callback function.
  cerata::logger().enable(fletchgen::LogCerata);

  // Parse options
  auto options = std::make_shared<fletchgen::Options>();
  if (!fletchgen::Options::Parse(options.get(), argc, argv)) {
    FLETCHER_LOG(ERROR, "Error parsing arguments. Exiting Fletchgen.");
    return -1;
  }

  // Quit the program early.
  if (options->quit) {
    fletcher::StopLogging();
    return 0;
  }

  // The resulting design.
  fletchgen::Design design;
  // Potential RecordBatch descriptors for simulation models.
  std::vector<fletcher::RecordBatchDescription> srec_batch_desc;

  // Generate designs in Cerata
  if (options->MustGenerateDesign()) {
    design = fletchgen::Design::GenerateFrom(options);
  } else {
    FLETCHER_LOG(ERROR, "No schemas detected. Cannot generate design.");
  }

  // Generate SREC output
  if (options->MustGenerateSREC()) {
    FLETCHER_LOG(INFO, "Generating SREC output.");
    auto srec_out = std::ofstream(options->srec_out_path);
    fletchgen::srec::GenerateReadSREC(design.batch_desc, &srec_batch_desc, &srec_out, 64);
    srec_out.close();
  }

  // Generate DOT output
  if (options->MustGenerateDOT()) {
    FLETCHER_LOG(INFO, "Generating DOT output.");
    auto dot = cerata::dot::DOTOutputGenerator(options->output_dir, design.GetOutputSpec());
    dot.Generate();
  }

  // Generate VHDL output
  if (options->MustGenerateVHDL()) {
    FLETCHER_LOG(INFO, "Generating VHDL output.");
    auto vhdl = cerata::vhdl::VHDLOutputGenerator(options->output_dir,
                                                  design.GetOutputSpec(),
                                                  fletchgen::DEFAULT_NOTICE);
    vhdl.Generate();
  }

  // Generate simulation top level
  if (options->MustGenerateDesign() && options->sim_top) {
    std::ofstream sim_file;
    std::string sim_file_path = options->output_dir + "/vhdl/SimTop_tc.vhd";
    if (cerata::FileExists(sim_file_path) && !options->overwrite) {
      sim_file_path += 't';
    }
    FLETCHER_LOG(INFO, "Saving simulation top-level design to: " + sim_file_path);
    sim_file = std::ofstream(sim_file_path);
    // If the srec simulation dump path doesn't exist, it can't be canonicalized later on.
    if (!cerata::FileExists(options->srec_sim_dump)) {
      // Just touch the file.
      std::ofstream srec_out(options->srec_sim_dump);
      srec_out.close();
    }
    fletchgen::top::GenerateSimTop(*design.mantle,
                                   {&sim_file},
                                   options->srec_out_path,
                                   options->srec_sim_dump,
                                   srec_batch_desc);
    sim_file.close();
  }

  // Generate AXI top level
  if (options->axi_top) {
    std::ofstream axi_file;
    std::string axi_file_path = options->output_dir + "/vhdl/AxiTop.vhd";
    if (cerata::FileExists(axi_file_path) && !options->overwrite) {
      axi_file_path += 't';
    }
    FLETCHER_LOG(INFO, "Saving AXI top-level design to: " + axi_file_path);
    axi_file = std::ofstream(axi_file_path);
    fletchgen::top::GenerateAXITop(*design.mantle, {&axi_file});
    axi_file.close();
  }

  // Generate Vivado HLS template
  if (options->vivado_hls) {
    FLETCHER_LOG(WARNING, "Vivado HLS template output not yet implemented.");
    /*
    auto hls_template_path = options->output_dir + "/vivado_hls/" + options->kernel_name + ".cpp";
    FLETCHER_LOG(INFO, "Generating Vivado HLS output: " + hls_template_path);
    cerata::CreateDir(options->output_dir + "/vivado_hls");
    auto hls_template_file = std::ofstream(hls_template_path);
    hls_template_file << fletchgen::hls::GenerateVivadoHLSTemplate(*design.kernel);
    */
  }

  FLETCHER_LOG(INFO, program_name + " completed.");

  // Shut down logging
  fletcher::StopLogging();
  return 0;
}

}  // namespace fletchgen
