#pragma once
// fastmm::cli::data: the fastmm-data command line as a library function. The fastmm-data app
// is `return fastmm::cli::data(argc, argv);`.
//
// It lists the registered market-data sources (backtest/data_registry.hpp) and converts any of
// them into an .fmj journal, which is what a backtest replays fastest. Downloading the files a
// source reads is `python3 -m fastmm.data fetch`. Run `--help` for the flags and exit codes.

namespace fastmm::cli {

int data(int argc, char** argv);

}  // namespace fastmm::cli
