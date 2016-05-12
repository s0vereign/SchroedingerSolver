#include <vector>
#include <iostream>
#include <omp.h>
#include "Numerov.hpp"


Numerov::Numerov():nx(0),ne(0),xmax(0),xmin(0){}

Numerov::Numerov(Params1D *pa): param(pa),
                                chunk(pa->getnx()*CHUNKSIZE),
                                cache(pa->getnx()*CHUNKSIZE),
                                nx(pa->getnx()),
                                ne(pa->getne()),
                                z(pa->getz()),
                                xmax(pa->getxmax()),
                                xmin(pa->getxmin()) {

    // initialize the cache, with the inital values
    for(auto it = cache.begin(); it != cache.end(); it += nx) {
        // the first entry always has to be zero, the second
        // is an arbitrary small value
        *(it) = 0;
        *(it+1) = 1e-10;
    }
    //get the minimal energy E_n is in [V(0,0,z),0]
    //We'll define it as positive
    //Kernel code then uses negaive values!
    E = V(0,0,z);
}

Numerov::~Numerov(){}

void Numerov::solve(){
    // This Loop is used to create
    for(auto j = 1; j < 2; j++) {
        z = j;
        DEBUG("Solving for Z ="<<z)

        double *dev_ptr;

        int dev_ne = 0;
        // Next let's allocate the required chunk memory on the device side
        cudaMalloc((void **) &dev_ptr, sizeof(double) * nx * CHUNKSIZE);
        // Make use of some local variables
        int index = 0;
        double dE = V(0, 0, z) / (double) ne;
        // This will be the starting energy for each chunk calculation
        double En = 0.0;
        while (index < ne) {
            //copy initals on device
            dev_ne = CHUNKSIZE;
            if (index + CHUNKSIZE > ne) {
                dev_ne = ne - index;
                cudaFree(dev_ptr);
                cudaMalloc((void **) &dev_ptr, sizeof(double) * nx * dev_ne);
            }
            DEBUG2("Calculating chunk: " << index / CHUNKSIZE);
            cudaMemcpy(dev_ptr, cache.data(), sizeof(double) * nx * dev_ne, cudaMemcpyHostToDevice);
            En = dE * (double) index ;
            DEBUG2("Calculating with starting energy: " << -En);
            iter1 <<< dev_ne, 1 >>> (dev_ptr, nx, dev_ne, xmax, xmin, z, En, dE);
            cudaMemcpy(chunk.data(), dev_ptr, sizeof(double) * nx * dev_ne, cudaMemcpyDeviceToHost);
            if(bisect(En)) index = ne;
            index += CHUNKSIZE;

        }
    }
    // After all the calculations done we can save our energy levels!
    prepstates();
    savelevels();
}

void Numerov::savelevels(){
    // Function to provide saving functionality of the energy Levels
    // First Allocate an appropiate vector

    DEBUG(res.size()/nx)

    hid_t file_id;
    vector<double> buffer2(eval.size());

    for(auto it = 0; it < eval.size(); it++) {
        // We do the analog thing for the enegy
        // It's a lot simpler!
        buffer2[it] = eval.back();
        eval.pop_back();
    }
    // Create a new HDF5 file

    file_id = H5Fcreate("sim1.h5",H5F_ACC_TRUNC,H5P_DEFAULT,H5P_DEFAULT);

    hsize_t dims = res.size();

    // Create a HDF5 Data set and write buffer1
    H5LTmake_dataset(file_id, "/numres", 1, &dims, H5T_NATIVE_DOUBLE, res.data());
    // Analog for buffer2
    dims = buffer2.size();
    H5LTmake_dataset(file_id, "/evals", 1, &dims, H5T_NATIVE_DOUBLE, buffer2.data());
    //Save some necessary parameters
    vector<double> p(3);
    p[0] = nx;
    p[1] = ne;
    p[2] = xmax;
    dims = 3 ;
    H5LTmake_dataset( file_id, "/params", 1, &dims, H5T_NATIVE_DOUBLE, p.data());
    // close the file
    H5Fclose(file_id);
}


bool Numerov::sign(double s){
    return std::signbit(s);
}



int Numerov::bisect(double j) {
    // Iterate through chunk data
    // create local variable for the offset
    // off is the index of the last Element
    const int off = nx - 1;
    auto it = chunk.begin();
    vector<double> temp( nx);
    double dE = -V(0,0,z)/ne;
    double En = 0;
//#pragma omp parallel for
    for( auto i = 2 * off; i < chunk.size(); i += nx){
        if(sign( chunk[ i ]) != sign( chunk[ i - nx])){
            if( (fabs( chunk[ i]) < fabs( chunk[i - nx]))&&chunk[i]<1e-3) {
                res.resize(res.size()+nx);
                auto iter = res.end()-nx;
                std::copy(it+i,it+i+nx,iter);
                En = ( -j + i/nx * dE);
                std::cout << "Energy level found" << std::endl;
                std::cout << "Detected energy level: "<< En << std::endl;
                eval.push_back(En);
                //return 1;
            }
            else {
                if(chunk[i-nx]<1e-3) {
                    res.resize(res.size()+nx);
                    auto iter = res.end()-nx;
                    std::copy(it+i,it+i+nx,iter);
                    En = (-j + (i/nx - 1) * dE);
                    std::cout << "Energy level found" << std::endl;
                    std::cout << "Detected energy level: "<< En << std::endl;
                    eval.push_back(En);

                  //  return 1;
                }
            }
        }
    }
    return 0;
}

double Numerov::trapez(int first, int last) {

    double h = (xmax - xmin) / (double) nx;
    auto temp = 0.0;
    for(auto i = first; i < last; ++i){
        temp += res[i]*res[i] * 2.0;
    }
    temp -= (res[first]*res[first] + res[last]*res[last]);
    temp *= h/2;
    return 1/sqrt(temp);
}

void Numerov::mult_const(int first, int last, double c) {

    for( auto i = first; i < last; ++i) {
        res[i] *= c;
    }
}

void Numerov::prepstates() {
    // This function normalizes the
    // static Solutions of the Numerov solution and
    // Writes them to the

    double c_temp = 0;
    for(auto i = 0; i < res.size(); i += nx+1) {
        c_temp = trapez(i, i+nx);
        mult_const( i, i+nx, c_temp);
    }
}

void Numerov::copystate(int ind, thrust::host_vector<cuDoubleComplex>& v) {

    int o = nx*ind;
    for(auto i = 0; i < nx; ++i) {
        v[i] = make_cuDoubleComplex(res[i + o], 0);
    }
}