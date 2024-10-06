# frozen_string_literal: true

require 'rubygems'
require 'mkmf'
require 'rbconfig'

dir_config 'um_ext'

KERNEL_INFO_RE = /Linux (\d)\.(\d+)(?:\.)?((?:\d+\.?)*)(?:\-)?([\w\-]+)?/
def get_config
  if RUBY_PLATFORM !~ /linux/
    raise "UringMachine only works on Linux!"
  end

  kernel_info = `uname -sr`
  m = kernel_info.match(KERNEL_INFO_RE)
  raise "Could not parse Linux kernel information (#{kernel_info.inspect})" if !m

  version, major_revision, distribution = m[1].to_i, m[2].to_i, m[4]

  combined_version = version.to_i * 100 + major_revision.to_i
  raise "UringMachine requires kernel version 6.4 or newer!" if combined_version < 604

  {
    kernel_version:     combined_version,
    submit_all_flag:    combined_version >= 518,
    coop_taskrun_flag:  combined_version >= 519,
    single_issuer_flag: combined_version >= 600,
    prep_bind:          combined_version >= 611,
    prep_listen:        combined_version >= 611,
    prep_cmd_sock:      combined_version >= 608  
  }
end

config = get_config
puts "Building UringMachine (\n#{config.map { |(k, v)| "  #{k}: #{v}\n"}.join})"

# require_relative 'zlib_conf'

liburing_path = File.expand_path('../../vendor/liburing', __dir__)
FileUtils.cd liburing_path do
  system('./configure', exception: true)
  FileUtils.cd File.join(liburing_path, 'src') do
    system('make', 'liburing.a', exception: true)
  end
end

if !find_header 'liburing.h', File.join(liburing_path, 'src/include')
  raise "Couldn't find liburing.h"
end

if !find_library('uring', nil, File.join(liburing_path, 'src'))
  raise "Couldn't find liburing.a"
end

$defs << '-DHAVE_IORING_SETUP_SUBMIT_ALL'   if config[:submit_all_flag]
$defs << '-DHAVE_IORING_SETUP_COOP_TASKRUN' if config[:coop_taskrun_flag]
$defs << '-DHAVE_IO_URING_PREP_BIND'        if config[:prep_bind]
$defs << '-DHAVE_IO_URING_PREP_LISTEN'      if config[:prep_listen]
$defs << '-DHAVE_IO_URING_PREP_CMD_SOCK'    if config[:prep_cmd_sock]
$CFLAGS << ' -Wno-pointer-arith'

CONFIG['optflags'] << ' -fno-strict-aliasing'

create_makefile 'um_ext'
