// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements the wiring.
#include "app/app.hpp"

#include "core/errors.hpp"
#include "services.hpp"

namespace azm::app {

struct App::Impl {
  Config cfg;
};

App::App(Config cfg) : impl_(std::make_unique<Impl>(Impl{std::move(cfg)})) {}
App::~App() = default;

void App::start() { throw NotImplemented("app::App::start"); }
void App::stop() {}
int App::run() { throw NotImplemented("app::App::run"); }
uint16_t App::port() const { throw NotImplemented("app::App::port"); }
Services& App::services() { throw NotImplemented("app::App::services"); }
const Config& App::config() const { return impl_->cfg; }

}  // namespace azm::app
