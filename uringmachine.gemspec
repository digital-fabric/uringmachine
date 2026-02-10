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
  s.extra_rdoc_files = `git ls-files -z *.rdoc *.md lib/*.rb lib/**/*.rb ext/um/*.c ext/um/*.h benchmark/*.md benchmark/*.png`.split("\x0")
  s.extensions = ["ext/um/extconf.rb"]
  s.require_paths = ["lib"]
  s.required_ruby_version = '>= 3.5'
end
