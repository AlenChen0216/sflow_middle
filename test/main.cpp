extern "C" {
#include <nfp.h>
#include <nfp_cpp.h>
#include <nfp_nffw.h>
}

#include <iostream>
#include <vector>
using namespace std;


int main(){
    auto dev_ = nfp_device_open(0);
    auto cpp_ = nfp_device_cpp(dev_);
    const struct nfp_rtsym *rtsym ;
    for(int i=0; i<nfp_rtsym_count(dev_); i++){
        rtsym = nfp_rtsym_get(dev_, i);
        if(strcmp(rtsym->name, "_pif_register_reg_src_ip") == 0){
            cout<<"Found symbol: \n";
            break;
        }
        rtsym = nullptr;
    }
    if(rtsym!= nullptr){
        cout<<"Detail of symbol:\n";
        cout<<"Name: "<<rtsym->name<<"\n";
        cout<<"Address: "<<hex<<rtsym->addr<<"\n";
        cout<<"Size: "<<dec<<rtsym->size<<"\n";
        cout<<"Type: "<<rtsym->type<<"\n";
        cout<<"Target: "<<rtsym->target<<"\n";
        cout<<"Domain: "<<rtsym->domain<<"\n";
    }
    nfp_device_close(dev_);
}