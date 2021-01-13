#include "f2718.hh"

f2718::f2718(std::shared_ptr<MongoLog>& log, CrateOptions opt, int link, int crate) :
    V2718(log, opt, link, crate) {}

f2718::~f2718() {
    fLog.reset();
}
