#include "f2718.hh"

f2718::f2718(std::shared_ptr<MongoLog>& log, CrateOptions opt) :
    V2718(log, opt) {}

f2718::~f2718() {
    fLog.reset();
}
