require_relative './lib/uringmachine/version'

Gem::Specification.new do |s|
  s.name        = 'uringmachine'
  s.version     = UringMachine::VERSION
  s.licenses    = ['MIT']
  s.summary     = 'A lean, mean io_uring machine'
  s.author      = 'Sharon Rosner'
  s.email       = 'sharon@noteflakes.com'
  s.files       = `git ls-files --recurse-submodules`.split.reject { |fn| fn =~ /liburing\/man/ }
  s.homepage    = 'https://github.com/digital-fabric/uringmachine'
  s.metadata    = {
    "source_code_uri" => "https://github.com/digital-fabric/uringmachine",
    "documentation_uri" => "https://www.rubydoc.info/gems/uringmachine",
    "changelog_uri" => "https://github.com/digital-fabric/uringmachine/blob/master/CHANGELOG.md"
  }
  s.rdoc_options = ["--title", "UringMachine", "--main", "README.md"]
  s.extra_rdoc_files = ["README.md"]
  s.extensions = ["ext/um/extconf.rb"]
  s.require_paths = ["lib"]
  s.required_ruby_version = '>= 3.3'

  s.add_development_dependency  'rake-compiler',        '1.2.8'
  s.add_development_dependency  'minitest',             '5.25.1'
  s.add_development_dependency  'http_parser.rb',       '0.8.0'
  s.add_development_dependency  'benchmark-ips',        '2.14.0'
  s.add_development_dependency  'localhost',            '1.3.1'
end
