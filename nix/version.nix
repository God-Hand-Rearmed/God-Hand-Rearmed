# KAMITE_VERSION from src/kamite/version.h ("0.6.9.1"): the one place the version is written.
{ src }:
let
  found = builtins.match "(.|\n)*#define KAMITE_VERSION \"([^\"]+)\"(.|\n)*" (builtins.readFile "${src}/src/kamite/version.h");
in
if found == null then throw "src/kamite/version.h: no KAMITE_VERSION" else builtins.elemAt found 1
