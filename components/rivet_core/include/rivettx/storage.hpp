#pragma once

#include "rivettx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rivettx {

uint32_t crc32(const uint8_t* data, std::size_t size);

class ModelCodec {
 public:
  static std::vector<uint8_t> encode(const Model& model,
                                     uint32_t generation);
  static bool decode(const std::vector<uint8_t>& encoded, Model& model,
                     uint32_t& generation, std::string& error);

 private:
  static bool migrate(uint16_t source_schema, Model& model,
                      std::string& error);
};

class IFileStore {
 public:
  virtual ~IFileStore() = default;
  virtual bool read(const std::string& path,
                    std::vector<uint8_t>& data) const = 0;
  virtual bool write(const std::string& path,
                     const std::vector<uint8_t>& data) = 0;
  virtual bool rename(const std::string& from,
                      const std::string& to) = 0;
  virtual bool remove(const std::string& path) = 0;
  virtual bool exists(const std::string& path) const = 0;
  virtual bool sync(const std::string& path) = 0;
};

class PosixFileStore final : public IFileStore {
 public:
  explicit PosixFileStore(std::string root);

  bool read(const std::string& path,
            std::vector<uint8_t>& data) const override;
  bool write(const std::string& path,
             const std::vector<uint8_t>& data) override;
  bool rename(const std::string& from, const std::string& to) override;
  bool remove(const std::string& path) override;
  bool exists(const std::string& path) const override;
  bool sync(const std::string& path) override;

 private:
  std::string full_path(const std::string& path) const;
  std::string root_;
};

struct ModelLoadResult {
  bool success = false;
  bool recovered = false;
  uint32_t generation = 0;
  std::string source;
  std::string error;
};

class TransactionalModelStore {
 public:
  TransactionalModelStore(IFileStore& files, std::string base_path);

  bool save(const Model& model, uint32_t generation,
            std::string& error);
  ModelLoadResult load(Model& model);
  bool erase();
  bool export_active(std::vector<uint8_t>& data) const;
  bool import_candidate(const std::vector<uint8_t>& data,
                        std::string& error);

 private:
  bool decode_file(const std::string& path, Model& model,
                   uint32_t& generation, std::string& error) const;

  IFileStore& files_;
  std::string active_;
  std::string temporary_;
  std::string backup_;
};

class CalibrationStore {
 public:
  CalibrationStore(IFileStore& files, std::string path);
  bool save(const std::array<AxisCalibration, kMaxAxes>& calibration);
  bool load(std::array<AxisCalibration, kMaxAxes>& calibration) const;

 private:
  IFileStore& files_;
  std::string path_;
};

}  // namespace rivettx
