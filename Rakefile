# frozen_string_literal: true

require "bundler/gem_tasks"
require "rake/clean"
require "rake/testtask"
require "rake/extensiontask"

Rake::ExtensionTask.new("um_ext") do |ext|
  ext.ext_dir = "ext/um"
end

task :recompile => [:clean, :compile]
task :default => [:compile, :test]

test_config = -> (t) {
  t.libs << "test"
  t.test_files = FileList["test/**/test_*.rb"]
}
Rake::TestTask.new(test: :compile, &test_config)

task :stress_test do
  exec 'ruby test/stress.rb'
end

CLEAN.include "**/*.o", "**/*.so", "**/*.so.*", "**/*.a", "**/*.bundle", "**/*.jar", "pkg", "tmp"

task :release do
  require_relative './lib/uringmachine/version'
  version = UringMachine::VERSION

  puts 'Building uringmachine...'
  `gem build uringmachine.gemspec`

  puts "Pushing uringmachine #{version}..."
  `gem push uringmachine-#{version}.gem`

  puts "Cleaning up..."
  `rm *.gem`
end

require 'yard'
YARD_FILES = FileList['ext/um/*.c', 'lib/uringmachine.rb', 'lib/uringmachine/**/*.rb']

YARD::Rake::YardocTask.new do |t|
  t.files   = YARD_FILES
  t.options = %w( --verbose -o yard --readme README.md)
end
