#include <iostream>
#include "TFile.h"
#include "TTree.h"
#include "../TDataFrame.hxx"
#include "Sentinel.h"
// test objects read from tree are constructed once per entry
void FillTree(const char* filename, const char* treeName) {
   TFile f(filename, "RECREATE");
   TTree t(treeName, treeName);
   Sentinel o;
   t.Branch("obj", &o);
   o.set(1);
   t.Fill();
   o.set(2);
   t.Fill();
   t.Write();
   f.Close();
}

void test_ctors() {
   // Prepare an input tree to run on
   auto fileName = "myfile.root";
   auto treeName = "myTree";
   std::cout << "filling tree...\n";
   FillTree(fileName,treeName);
   std::cout << "done\n";
   TFile f(fileName);

   // Filter, AddBranch, Count and Foreach. We want one printout
   std::cout << "building dataframe...\n";
   TDataFrame d(treeName, &f, {"obj"});
   std::cout << "done\nbuilding chain...\n";
   auto dd = d.Filter([](const Sentinel& o) { std::cout << "filter\n"; return o.get() > 0; })
              .AddBranch("x", [](const Sentinel& o) { std::cout << "addbranch\n"; return o.get(); });
   auto r = dd.Count();
   dd.Foreach([](const Sentinel& o) { std::cout << "foreach: " << o.get() << std::endl; });
   std::cout << "done\n";
   std::cout << "running chain...\n";
   auto c = *r;
   std::cout << "done\n";
   std::cout << "count: " << c << std::endl;
}
