#include "DAOSConnectorImpl.h"

class DAOSConnector {
public:
    DAOSConnector(const char* pool_label, const char* cont_label)
        : pImpl(new DAOSConnectorImpl(pool_label, cont_label)) {}

    ~DAOSConnector() {
        delete pImpl;
    }

    float getPoolUsage() {
        return pImpl->getPoolUsage();
    }

    void printConnectorName() const {
        pImpl->printConnectorName();
    }

    void processError(const char* function_name, const int err_num) {
        pImpl->processError(function_name, err_num);
    }

protected:
    daos_handle_t getPoh() const {
        return pImpl->getPoh();
    }

    daos_handle_t getCoh() const {
        return pImpl->getCoh();
    }

private:
    DAOSConnectorImpl* pImpl;
};
