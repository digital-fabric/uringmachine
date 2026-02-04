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
  raise "UringMachine requires kernel version 6.7 or newer!" if combined_version < 607

  {
    kernel_version:       combined_version,
    prep_bind:            combined_version >= 611,
    prep_listen:          combined_version >= 611,
    send_vectoized:       combined_version >= 617
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

if !find_header 'liburing/io_uring.h', File.join(liburing_path, 'src/include')
  raise "Couldn't find liburing/io_uring.h"
end

if !find_library('uring', nil, File.join(liburing_path, 'src'))
  raise "Couldn't find liburing.a"
end

if !have_header("openssl/ssl.h")
  raise "Couldn't find OpenSSL headers"
end

if !have_library("ssl", "SSL_new")
  raise "Couldn't find OpenSSL library"
end

version_ok = checking_for("OpenSSL version >= 1.1.1") {
  try_static_assert("OPENSSL_VERSION_NUMBER >= 0x10101000L", "openssl/opensslv.h")
}
unless version_ok
  raise "OpenSSL >= 1.1.1 or LibreSSL >= 3.9.0 is required"
end

have_func("&rb_process_status_new")

$defs << "-DUM_KERNEL_VERSION=#{config[:kernel_version]}"
$defs << '-DHAVE_IO_URING_PREP_BIND'            if config[:prep_bind]
$defs << '-DHAVE_IO_URING_PREP_LISTEN'          if config[:prep_listen]
$defs << '-DHAVE_IO_URING_SEND_VECTORIZED'      if config[:send_vectoized]

$CFLAGS << ' -Wno-pointer-arith'

CONFIG['optflags'] << ' -fno-strict-aliasing'

create_makefile 'um_ext'
