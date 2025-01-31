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

#include <fletcher/common.h>
#include <cerata/api.h>

#include <string>

/// Contains all classes and functions related to Fletchgen.
namespace fletchgen {

/// @brief Return the name of this program executable.
std::string GetProgramName(char *argv0);

/// @brief Return a Cerata port direction from a Fletcher access mode.
cerata::Port::Dir mode2dir(fletcher::Mode mode);

/**
 * @brief Callback function for the Cerata logger.
 * @param level             The logging level.
 * @param message
 * @param source_function
 * @param source_file
 * @param line_number
 */
void LogCerata(cerata::LogLevel level,
               std::string const &message,
               char const *source_function,
               char const *source_file,
               int line_number);

/// Default copyright notice.
constexpr char DEFAULT_NOTICE[] = "-- Copyright 2018-2019 Delft University of Technology\n"
                                  "--\n"
                                  "-- Licensed under the Apache License, Version 2.0 (the \"License\");\n"
                                  "-- you may not use this file except in compliance with the License.\n"
                                  "-- You may obtain a copy of the License at\n"
                                  "--\n"
                                  "--     http://www.apache.org/licenses/LICENSE-2.0\n"
                                  "--\n"
                                  "-- Unless required by applicable law or agreed to in writing, software\n"
                                  "-- distributed under the License is distributed on an \"AS IS\" BASIS,\n"
                                  "-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"
                                  "-- See the License for the specific language governing permissions and\n"
                                  "-- limitations under the License.\n"
                                  "--\n"
                                  "-- This file was generated by Fletchgen. Modify this file at your own risk.\n"
                                  "\n";

}  // namespace fletchgen

