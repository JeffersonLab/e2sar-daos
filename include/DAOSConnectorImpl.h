#include <daos.h>  // Include DAOS headers as necessary

class DAOSConnectorImpl {
public:
    DAOSConnectorImpl(const char* pool_label, const char* cont_label);
    ~DAOSConnectorImpl();

    daos_handle_t getPoh() const;
    daos_handle_t getCoh() const;

    float getPoolUsage();
    void printConnectorName() const;
    void processError(const char* function_name, const int err_num);

protected:
    daos_handle_t _poh = DAOS_HDL_INVAL;  // DAOS handle for the Pool.
    daos_handle_t _coh = DAOS_HDL_INVAL;  // DAOS handle for the container.

private:
    const char* pool_label;
    const char* cont_label;

    void initDAOSConnection();
    void closeDAOSConnection();
};
