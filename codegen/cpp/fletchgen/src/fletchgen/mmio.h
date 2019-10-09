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

#pragma once

#include <fletcher/arrow-utils.h>
#include <cerata/api.h>
#include <string>
#include <memory>
#include <utility>
#include <vector>
#include <unordered_map>

namespace fletchgen {

using cerata::Component;
using cerata::Instance;
using cerata::Node;
using cerata::Literal;
using cerata::Port;
using cerata::Parameter;
using cerata::PortArray;
using cerata::integer;
using cerata::Type;
using cerata::ClockDomain;

/// Fletchgen metadata for mmio-controlled buffer address ports.
constexpr char MMIO_DEFAULT[] = "fletchgen_mmio_default";
/// Fletchgen metadata for mmio-controlled buffer address ports.
constexpr char MMIO_BATCH[] = "fletchgen_mmio_batch";
/// Fletchgen metadata for mmio-controlled buffer address ports.
constexpr char MMIO_BUFFER[] = "fletchgen_mmio_buffer";
/// Fletchgen metadata for mmio-controlled kernel ports.
constexpr char MMIO_KERNEL[] = "fletchgen_mmio_kernel";
/// Fletchgen metadata for mmio-controlled profiling ports.
constexpr char MMIO_PROFILE[] = "fletchgen_mmio_profile";

/// @brief Structure to represent an mmio register
struct MmioReg {
  /// Register intended use enumeration.
  enum class Function {
    DEFAULT,   ///< Default registers.
    BATCH,     ///< Registers for RecordBatch metadata.
    BUFFER,    ///< Registers for buffer addresses.
    KERNEL,    ///< Registers for the kernel.
    PROFILE    ///< Register for the profiler.
  } function;  ///< Register intended use.
  /// Register access behavior enumeration.
  enum class Behavior {
    CONTROL,   ///< Register contents is controlled by host software.
    STATUS     ///< Register contents is controlled by hardware kernel.
  } behavior;  ///< Register access behavior.

  /// Name.
  std::string name;
  /// Description.
  std::string desc;
  /// Bit width.
  uint32_t width;
  /// Optional address.
  std::optional<uint32_t> addr = std::nullopt;
  /// LSB start index at that address.
  uint32_t index = 0;
  /// Metadata.
  std::unordered_map<std::string, std::string> meta;
};

bool ExposeToKernel(MmioReg::Function fun);

/**
 * @brief A port on the vhdmmio component. Remembers what register spec it came from.
 */
struct MmioPort : public Port {
  MmioPort(const std::string &name, Port::Dir dir, const MmioReg &reg = {},
           const std::shared_ptr<ClockDomain> &domain = cerata::default_domain());
  MmioReg reg;
  std::shared_ptr<Object> Copy() const override {
    return std::make_shared<MmioPort>(name(), dir_, reg, domain_);
  }
};

/// @brief Create an mmio port.
std::shared_ptr<MmioPort> mmio_port(Port::Dir dir, const MmioReg &reg,
                                    const std::shared_ptr<ClockDomain> &domain = cerata::default_domain());

/**
 * @brief Returns a YAML string for the vhdmmio tool based on a set of registers.
 *
 * Any fixed addresses in the MmioReg.address field can only occur at the start of the vector and must be ordered.
 *
 * @param regs       The set of registers. Any resolved addresses will be written back to the registers.
 * @param next_addr  Optionally outputs the byte address offset of the next free register address.
 */
std::string GenerateVhdmmioYaml(std::vector<MmioReg> *regs, std::optional<size_t *> next_addr = std::nullopt);

/**
 * @brief Generate the MMIO component for the nucleus.
 *
 * Must generate the component in such a way that GenerateVhdmmioYaml in combination with the vhdmmio tool creates
 * an identical component interface.
 *
 * @param[in]  batches         The RecordBatchDescriptions of the recordbatches in the design.
 * @param[in]  regs     A list of custom 32-bit register names.
 * @param[out] buf_port_names  The resulting port names of all buffers.
 * @return A component.
 */
std::shared_ptr<Component> mmio(const std::vector<fletcher::RecordBatchDescription> &batches,
                                const std::vector<MmioReg> &regs);

}  // namespace fletchgen
