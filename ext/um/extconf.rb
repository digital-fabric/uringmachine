# frozen_string_literal: true

require 'rubygems'
require 'mkmf'
require 'rbconfig'

dir_config 'um_ext'

def config_ssl
  # don't use pkg_config('openssl') if '--with-openssl-dir' is used
  has_openssl_dir = dir_config('openssl').any? ||
    RbConfig::CONFIG['configure_args']&.include?('openssl')

  found_pkg_config = !has_openssl_dir && pkg_config('openssl')

  found_ssl = if !$mingw && found_pkg_config
    puts '──── Using OpenSSL pkgconfig (openssl.pc) ────'
    true
  elsif have_library('libcrypto', 'BIO_read') && have_library('libssl', 'SSL_CTX_new')
    true
  elsif %w'crypto libeay32'.find {|crypto| have_library(crypto, 'BIO_read')} &&
      %w'ssl ssleay32'.find {|ssl| have_library(ssl, 'SSL_CTX_new')}
    true
  else
    puts '** Puma will be compiled without SSL support'
    false
  end

  if found_ssl
    have_header "openssl/bio.h"

    ssl_h = "openssl/ssl.h".freeze

    puts "\n──── Below are yes for 1.0.2 & later ────"
    have_func "DTLS_method"                            , ssl_h
    have_func "SSL_CTX_set_session_cache_mode(NULL, 0)", ssl_h

    puts "\n──── Below are yes for 1.1.0 & later ────"
    have_func "TLS_server_method"                      , ssl_h
    have_func "SSL_CTX_set_min_proto_version(NULL, 0)" , ssl_h

    puts "\n──── Below is yes for 1.1.0 and later, but isn't documented until 3.0.0 ────"
    # https://github.com/openssl/openssl/blob/OpenSSL_1_1_0/include/openssl/ssl.h#L1159
    have_func "SSL_CTX_set_dh_auto(NULL, 0)"           , ssl_h

    puts "\n──── Below is yes for 1.1.1 & later ────"
    have_func "SSL_CTX_set_ciphersuites(NULL, \"\")"   , ssl_h

    puts "\n──── Below is yes for 3.0.0 & later ────"
    have_func "SSL_get1_peer_certificate"              , ssl_h

    puts ''

    # Random.bytes available in Ruby 2.5 and later, Random::DEFAULT deprecated in 3.0
    if Random.respond_to?(:bytes)
      $defs.push "-DHAVE_RANDOM_BYTES"
      puts "checking for Random.bytes... yes"
    else
      puts "checking for Random.bytes... no"
    end
  end
end

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
    kernel_version:       combined_version,
    submit_all_flag:      combined_version >= 518,
    coop_taskrun_flag:    combined_version >= 519,
    single_issuer_flag:   combined_version >= 600,
    prep_bind:            combined_version >= 611,
    prep_listen:          combined_version >= 611,
    prep_cmd_sock:        combined_version >= 607,
    prep_futex:           combined_version >= 607,
    prep_waitid:          combined_version >= 607,
    prep_read_multishot:  combined_version >= 607
  }
end

config_ssl

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

$defs << "-DUM_KERNEL_VERSION=#{config[:kernel_version]}"
$defs << '-DHAVE_IORING_SETUP_SUBMIT_ALL'       if config[:submit_all_flag]
$defs << '-DHAVE_IORING_SETUP_COOP_TASKRUN'     if config[:coop_taskrun_flag]
$defs << '-DHAVE_IO_URING_PREP_BIND'            if config[:prep_bind]
$defs << '-DHAVE_IO_URING_PREP_LISTEN'          if config[:prep_listen]
$defs << '-DHAVE_IO_URING_PREP_CMD_SOCK'        if config[:prep_cmd_sock]
$defs << '-DHAVE_IO_URING_PREP_FUTEX'           if config[:prep_futex]
$defs << '-DHAVE_IO_URING_PREP_WAITID'          if config[:prep_waitid]
$defs << '-DHAVE_IO_URING_PREP_READ_MULTISHOT'  if config[:prep_read_multishot]
$CFLAGS << ' -Wno-pointer-arith'

CONFIG['optflags'] << ' -fno-strict-aliasing'

create_makefile 'um_ext'
