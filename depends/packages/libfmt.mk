package=libfmt
$(package)_version=7.1.3
$(package)_download_path=https://github.com/fmtlib/fmt/archive/
$(package)_file_name=$($(package)_version).tar.gz
$(package)_sha256_hash=5cae7072042b3043e12d53d50ef404bbb76949dad1de368d7f993a15c8c05ecc

# libmw uses fmt header-only (FMT_HEADER_ONLY in mw/util/StringUtil.h), so only the headers are staged.
define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/include && \
  cp -r include/fmt $($(package)_staging_prefix_dir)/include/
endef
