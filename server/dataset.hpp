// SPDX-License-Identifier: GPL-2.0-or-later
//
// The datasetId (protocol 4, WELCOME): "<engine>/<ephemeris>/<catalogs>#<8
// hex>", the digest over every data file's checksum and the engine version.
// It MUST change whenever any answer the server gives could change (3.4), so
// clients key their caches on it and may pin it. The digest is taken over the
// files' CONTENTS, not their names or times: a refreshed catalog with new
// orbits is a new dataset even under the same file name.
#ifndef PROMETHEIA_SERVER_DATASET_HPP
#define PROMETHEIA_SERVER_DATASET_HPP

#include <string>
#include <vector>

namespace prometheia::server {

struct Dataset {
    std::string engine;    // WELCOME's engine string, e.g. "Prometheia 0.1.0, JPL DE440"
    std::string ephemeris; // the ephemeris file's display name
    std::vector<std::string> catalogs; // display names, in the order they were added
    std::string id;                    // "<engine>/<ephemeris>/<catalogs>#<8 hex>"
};

// `engine` names the engine and what it reads (Engine::source()); the digest
// is over that plus every file's contents, name-tagged so that reordering or
// replacing a file with another of the same size cannot collide.
Dataset make_dataset(std::string engine, const std::string& ephemeris_path,
                     const std::vector<std::string>& catalog_paths,
                     const std::string& perturbers_path);

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_DATASET_HPP
