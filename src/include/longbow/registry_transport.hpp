#ifndef FLECHA_LONGBOW_REGISTRY_TRANSPORT_HPP
#define FLECHA_LONGBOW_REGISTRY_TRANSPORT_HPP

#include <string>

namespace flecha::longbow {

class RegistryTransport final {
   public:
    RegistryTransport();

    [[nodiscard]] const std::string& Base() const { return _base; }
    [[nodiscard]] std::string Read(const std::string& resource) const;
    [[nodiscard]] std::string Login() const;
    [[nodiscard]] std::string WhoAmI() const;
    void Logout() const;
    void Publish(const std::string& identity, const std::string& version,
                 const std::string& checksum, const std::string& archive) const;
    void Yank(const std::string& identity, const std::string& version) const;

   private:
    [[nodiscard]] bool Local() const;
    [[nodiscard]] std::string Token() const;
    std::string _base;
};

}  // namespace flecha::longbow

#endif
