// Copyright 2018-2019 Delft University of Technology
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

#include "fletchgen/axi4_lite.h"

#include <cerata/api.h>

#include "fletchgen/utils.h"

namespace fletchgen {

using cerata::Record;
using cerata::Stream;
using cerata::Vector;
using cerata::RecField;

std::shared_ptr<Type> axi4_lite_type(Axi4LiteSpec spec) {
  auto axi_typename = spec.ToAxiTypeName();
  auto opt_existing_axi_typename = cerata::default_type_pool()->Get(axi_typename);
  if (opt_existing_axi_typename) {
    // AXI-lite type already exists in type pool, just return that.
    return opt_existing_axi_typename.value()->shared_from_this();
  } else {
    auto new_axi_lite_type = Record::Make(axi_typename, {
        NoSep(RecField::Make("aw", Stream::Make(Record::Make("aw", {
            RecField::Make("addr", Vector::Make(spec.addr_width))})))),
        NoSep(RecField::Make("w", Stream::Make(Record::Make("w", {
            RecField::Make("data", Vector::Make(spec.data_width)),
            RecField::Make("strb", Vector::Make(spec.data_width / 8))
        })))),
        NoSep(RecField::Make("b", Stream::Make(Record::Make("b", {
            RecField::Make("resp", Vector::Make(2))})), true)),
        NoSep(RecField::Make("ar", Stream::Make(Record::Make("ar", {
            RecField::Make("addr", Vector::Make(spec.addr_width))
        })))),
        NoSep(RecField::Make("r", Stream::Make(Record::Make("r", {
            RecField::Make("data", Vector::Make(spec.data_width)),
            RecField::Make("resp", Vector::Make(2))})), true)),
    });
    cerata::default_type_pool()->Add(new_axi_lite_type);
    return new_axi_lite_type;
  }
}

std::string Axi4LiteSpec::ToString() const {
  std::stringstream str;
  str << "MmioSpec[";
  str << "addr:" << addr_width;
  str << ", dat:" << data_width;
  str << "]";
  return str.str();
}

std::string Axi4LiteSpec::ToAxiTypeName() const {
  std::stringstream str;
  str << "MMIO";
  str << "_A" << addr_width;
  str << "_D" << data_width;
  return str.str();
}

std::shared_ptr<Axi4LitePort> axi4_lite(Port::Dir dir,
                                        const std::shared_ptr<ClockDomain> &domain,
                                        Axi4LiteSpec spec) {
  return std::make_shared<Axi4LitePort>(dir, spec, "mmio", domain);
}

Axi4LitePort::Axi4LitePort(Port::Dir dir, Axi4LiteSpec spec, std::string name, std::shared_ptr<ClockDomain> domain) :
    Port(std::move(name), axi4_lite_type(spec), dir, std::move(domain)), spec_(spec) {}
}  // namespace fletchgen
