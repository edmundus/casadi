/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "symbolic_ocp.hpp"

#include <map>
#include <string>
#include <sstream>
#include <ctime>

#include "../std_vector_tools.hpp"
#include "../casadi_exception.hpp"
#include "../matrix/matrix_tools.hpp"
#include "../sx/sx_tools.hpp"
#include "../function/integrator.hpp"
#include "../function/code_generator.hpp"
#include "../casadi_calculus.hpp"
#include "xml_file.hpp"

using namespace std;
namespace casadi {

  SymbolicOCP::SymbolicOCP() {
    t = SX::sym("t");
    t0 = t0_guess = numeric_limits<double>::quiet_NaN();
    tf = tf_guess = numeric_limits<double>::quiet_NaN();
    t0_free = false;
    tf_free = false;
  }

  void SymbolicOCP::parseFMI(const std::string& filename) {

    // Load
    XmlFile xml_file("tinyxml");
    XmlNode document = xml_file.parse(filename);

    // **** Add model variables ****
    {
      //if (verbose) cout << "Adding model variables." << endl;

      // Get a reference to the ModelVariables node
      const XmlNode& modvars = document[0]["ModelVariables"];

      // Add variables
      for (int i=0; i<modvars.size(); ++i) {

        // Get a reference to the variable
        const XmlNode& vnode = modvars[i];

        // Get the attributes
        string name        = vnode.getAttribute("name");
        int valueReference;
        vnode.readAttribute("valueReference", valueReference);
        string variability = vnode.getAttribute("variability");
        string causality   = vnode.getAttribute("causality");
        string alias       = vnode.getAttribute("alias");

        // Skip to the next variable if its an alias
        if (alias.compare("alias") == 0 || alias.compare("negatedAlias") == 0)
          continue;

        // Get the name
        const XmlNode& nn = vnode["QualifiedName"];
        string qn = qualifiedName(nn);

        // Add variable, if not already added
        if (varmap_.find(qn)==varmap_.end()) {

          // Create variable
          Variable var(name);

          // Value reference
          var.valueReference = valueReference;

          // Variability
          if (variability.compare("constant")==0)
            var.variability = CONSTANT;
          else if (variability.compare("parameter")==0)
            var.variability = PARAMETER;
          else if (variability.compare("discrete")==0)
            var.variability = DISCRETE;
          else if (variability.compare("continuous")==0)
            var.variability = CONTINUOUS;
          else
            throw CasadiException("Unknown variability");

          // Causality
          if (causality.compare("input")==0)
            var.causality = INPUT;
          else if (causality.compare("output")==0)
            var.causality = OUTPUT;
          else if (causality.compare("internal")==0)
            var.causality = INTERNAL;
          else
            throw CasadiException("Unknown causality");

          // Alias
          if (alias.compare("noAlias")==0)
            var.alias = NO_ALIAS;
          else if (alias.compare("alias")==0)
            var.alias = ALIAS;
          else if (alias.compare("negatedAlias")==0)
            var.alias = NEGATED_ALIAS;
          else
            throw CasadiException("Unknown alias");

          // Other properties
          if (vnode.hasChild("Real")) {
            const XmlNode& props = vnode["Real"];
            props.readAttribute("unit", var.unit, false);
            props.readAttribute("displayUnit", var.displayUnit, false);
            props.readAttribute("min", var.min, false);
            props.readAttribute("max", var.max, false);
            props.readAttribute("initialGuess", var.initialGuess, false);
            props.readAttribute("start", var.start, false);
            props.readAttribute("nominal", var.nominal, false);
            props.readAttribute("free", var.free, false);
          }

          // Variable category
          if (vnode.hasChild("VariableCategory")) {
            string cat = vnode["VariableCategory"].getText();
            if (cat.compare("derivative")==0)
              var.category = CAT_DERIVATIVE;
            else if (cat.compare("state")==0)
              var.category = CAT_STATE;
            else if (cat.compare("dependentConstant")==0)
              var.category = CAT_DEPENDENT_CONSTANT;
            else if (cat.compare("independentConstant")==0)
              var.category = CAT_INDEPENDENT_CONSTANT;
            else if (cat.compare("dependentParameter")==0)
              var.category = CAT_DEPENDENT_PARAMETER;
            else if (cat.compare("independentParameter")==0)
              var.category = CAT_INDEPENDENT_PARAMETER;
            else if (cat.compare("algebraic")==0)
              var.category = CAT_ALGEBRAIC;
            else
              throw CasadiException("Unknown variable category: " + cat);
          }

          // Add to list of variables
          addVariable(qn, var);

          // Sort expression
          switch (var.category) {
          case CAT_DERIVATIVE:
            // Skip - meta information about time derivatives is
            //        kept together with its parent variable
            break;
          case CAT_STATE:
            this->s.push_back(var.v);
            this->sdot.push_back(var.d);
            break;
          case CAT_DEPENDENT_CONSTANT:
            // Skip
            break;
          case CAT_INDEPENDENT_CONSTANT:
            // Skip
            break;
          case CAT_DEPENDENT_PARAMETER:
            // Skip
            break;
          case CAT_INDEPENDENT_PARAMETER:
            if (var.free) {
              this->p.push_back(var.v);
            } else {
              // Skip
            }
            break;
          case CAT_ALGEBRAIC:
            if (var.causality == INTERNAL) {
              this->s.push_back(var.v);
              this->sdot.push_back(var.d);
            } else if (var.causality == INPUT) {
              this->u.push_back(var.v);
            }
            break;
          default:
            casadi_error("Unknown category");
          }
        }
      }
    }

    // **** Add binding equations ****
    {
      //if (verbose) cout << "Adding binding equations." << endl;

      // Get a reference to the BindingEquations node
      const XmlNode& bindeqs = document[0]["equ:BindingEquations"];

      for (int i=0; i<bindeqs.size(); ++i) {
        const XmlNode& beq = bindeqs[i];

        // Get the variable and binding expression
        Variable& var = readVariable(beq[0]);
        SX bexpr = readExpr(beq[1][0]);
        this->i.push_back(var.v);
        this->idef.push_back(bexpr);
      }
    }

    // **** Add dynamic equations ****
    {
      // Get a reference to the DynamicEquations node
      const XmlNode& dyneqs = document[0]["equ:DynamicEquations"];

      // Add equations
      for (int i=0; i<dyneqs.size(); ++i) {

        // Get a reference to the variable
        const XmlNode& dnode = dyneqs[i];

        // Add the differential equation
        SX de_new = readExpr(dnode[0]);
        this->dae.push_back(de_new);
      }
    }

    // **** Add initial equations ****
    {
      // Get a reference to the DynamicEquations node
      const XmlNode& initeqs = document[0]["equ:InitialEquations"];

      // Add equations
      for (int i=0; i<initeqs.size(); ++i) {

        // Get a reference to the node
        const XmlNode& inode = initeqs[i];

        // Add the differential equations
        for (int i=0; i<inode.size(); ++i) {
          this->init.push_back(readExpr(inode[i]));
        }
      }
    }

    // **** Add optimization ****
    if (document[0].hasChild("opt:Optimization")) {

      // Get a reference to the DynamicEquations node
      const XmlNode& opts = document[0]["opt:Optimization"];

      // Start time
      const XmlNode& intervalStartTime = opts["opt:IntervalStartTime"];
      if (intervalStartTime.hasChild("opt:Value"))
        intervalStartTime["opt:Value"].getText(t0);
      if (intervalStartTime.hasChild("opt:Free"))
        intervalStartTime["opt:Free"].getText(t0_free);
      if (intervalStartTime.hasChild("opt:InitialGuess"))
        intervalStartTime["opt:InitialGuess"].getText(t0_guess);

      // Terminal time
      const XmlNode& IntervalFinalTime = opts["opt:IntervalFinalTime"];
      if (IntervalFinalTime.hasChild("opt:Value"))
        IntervalFinalTime["opt:Value"].getText(tf);
      if (IntervalFinalTime.hasChild("opt:Free"))
        IntervalFinalTime["opt:Free"].getText(tf_free);
      if (IntervalFinalTime.hasChild("opt:InitialGuess"))
        IntervalFinalTime["opt:InitialGuess"].getText(tf_guess);

      // Time points
      const XmlNode& tpnode = opts["opt:TimePoints"];
      tp.resize(tpnode.size());
      for (int i=0; i<tp.size(); ++i) {
        // Get index
        int index;
        tpnode[i].readAttribute("index", index);

        // Get value
        double value;
        tpnode[i].readAttribute("value", value);
        tp[i] = value;
      }

      for (int i=0; i<opts.size(); ++i) {

        // Get a reference to the node
        const XmlNode& onode = opts[i];

        // Get the type
        if (onode.checkName("opt:ObjectiveFunction")) { // mayer term
          try {
            // Add components
            for (int i=0; i<onode.size(); ++i) {
              const XmlNode& var = onode[i];

              // If string literal, ignore
              if (var.checkName("exp:StringLiteral"))
                continue;

              // Read expression
              SX v = readExpr(var);
              this->mterm.push_back(v);
            }
          } catch(exception& ex) {
            throw CasadiException(std::string("addObjectiveFunction failed: ") + ex.what());
          }
        } else if (onode.checkName("opt:IntegrandObjectiveFunction")) {
          try {
            for (int i=0; i<onode.size(); ++i) {
              const XmlNode& var = onode[i];

              // If string literal, ignore
              if (var.checkName("exp:StringLiteral"))
                continue;

              // Read expression
              SX v = readExpr(var);
              this->lterm.push_back(v);
            }
          } catch(exception& ex) {
            throw CasadiException(std::string("addIntegrandObjectiveFunction failed: ")
                                  + ex.what());
          }
        } else if (onode.checkName("opt:IntervalStartTime")) {
          // Ignore, treated above
        } else if (onode.checkName("opt:IntervalFinalTime")) {
          // Ignore, treated above
        } else if (onode.checkName("opt:TimePoints")) {
          // Ignore, treated above
        } else if (onode.checkName("opt:PointConstraints")) {
          casadi_warning("opt:PointConstraints not supported, ignored");
        } else if (onode.checkName("opt:Constraints")) {
          casadi_warning("opt:Constraints not supported, ignored");
        } else if (onode.checkName("opt:PathConstraints")) {
          casadi_warning("opt:PointConstraints not supported, ignored");
        } else {
          casadi_warning("SymbolicOCP::addOptimization: Unknown node " << onode.getName());
        }
      }
    }

    // Make sure that the dimensions are consistent at this point
    casadi_assert_warning(this->s.size()==this->dae.size(),
                          "The number of differential-algebraic equations does not match "
                          "the number of implicitly defined states.");
    casadi_assert_warning(this->z.size()==this->alg.size(),
                          "The number of algebraic equations (equations not involving "
                          "differentiated variables) does not match the number of "
                          "algebraic variables.");
  }

  Variable& SymbolicOCP::readVariable(const XmlNode& node) {
    // Qualified name
    string qn = qualifiedName(node);

    // Find and return the variable
    return variable(qn);
  }

  SX SymbolicOCP::readExpr(const XmlNode& node) {
    const string& fullname = node.getName();
    if (fullname.find("exp:")== string::npos) {
      casadi_error("SymbolicOCP::readExpr: unknown - expression is supposed to "
                   "start with 'exp:' , got " << fullname);
    }

    // Chop the 'exp:'
    string name = fullname.substr(4);

    // The switch below is alphabetical, and can be thus made more efficient,
    // for example by using a switch statement of the first three letters,
    // if it would ever become a bottleneck
    if (name.compare("Add")==0) {
      return readExpr(node[0]) + readExpr(node[1]);
    } else if (name.compare("Acos")==0) {
      return acos(readExpr(node[0]));
    } else if (name.compare("Asin")==0) {
      return asin(readExpr(node[0]));
    } else if (name.compare("Atan")==0) {
      return atan(readExpr(node[0]));
    } else if (name.compare("Cos")==0) {
      return cos(readExpr(node[0]));
    } else if (name.compare("Der")==0) {
      const Variable& v = readVariable(node[0]);
      return v.d;
    } else if (name.compare("Div")==0) {
      return readExpr(node[0]) / readExpr(node[1]);
    } else if (name.compare("Exp")==0) {
      return exp(readExpr(node[0]));
    } else if (name.compare("Identifier")==0) {
      return readVariable(node).v;
    } else if (name.compare("IntegerLiteral")==0) {
      int val;
      node.getText(val);
      return val;
    } else if (name.compare("Instant")==0) {
      double val;
      node.getText(val);
      return val;
    } else if (name.compare("Log")==0) {
      return log(readExpr(node[0]));
    } else if (name.compare("LogLt")==0) { // Logical less than
      return readExpr(node[0]) < readExpr(node[1]);
    } else if (name.compare("LogGt")==0) { // Logical less than
      return readExpr(node[0]) > readExpr(node[1]);
    } else if (name.compare("Mul")==0) { // Multiplication
      return readExpr(node[0]) * readExpr(node[1]);
    } else if (name.compare("Neg")==0) {
      return -readExpr(node[0]);
    } else if (name.compare("NoEvent")==0) {
      // NOTE: This is a workaround, we assume that whenever NoEvent occurs,
      // what is meant is a switch
      int n = node.size();

      // Default-expression
      SX ex = readExpr(node[n-1]);

      // Evaluate ifs
      for (int i=n-3; i>=0; i -= 2) ex = if_else(readExpr(node[i]), readExpr(node[i+1]), ex);

      return ex;
    } else if (name.compare("Pow")==0) {
      return pow(readExpr(node[0]), readExpr(node[1]));
    } else if (name.compare("RealLiteral")==0) {
      double val;
      node.getText(val);
      return val;
    } else if (name.compare("Sin")==0) {
      return sin(readExpr(node[0]));
    } else if (name.compare("Sqrt")==0) {
      return sqrt(readExpr(node[0]));
    } else if (name.compare("StringLiteral")==0) {
      throw CasadiException(node.getText());
    } else if (name.compare("Sub")==0) {
      return readExpr(node[0]) - readExpr(node[1]);
    } else if (name.compare("Tan")==0) {
      return tan(readExpr(node[0]));
    } else if (name.compare("Time")==0) {
      return t.toScalar();
    } else if (name.compare("TimedVariable")==0) {
      return readVariable(node[0]).v;
    }

    // throw error if reached this point
    throw CasadiException(string("SymbolicOCP::readExpr: Unknown node: ") + name);

  }

  void SymbolicOCP::repr(std::ostream &stream, bool trailing_newline) const {
    stream << "Flat OCP";
    if (trailing_newline) stream << endl;
  }

  void SymbolicOCP::print(ostream &stream, bool trailing_newline) const {
    stream << "Dimensions: ";
    stream << "#s = " << this->s.size() << ", ";
    stream << "#x = " << this->x.size() << ", ";
    stream << "#z = " << this->z.size() << ", ";
    stream << "#q = " << this->q.size() << ", ";
    stream << "#i = " << this->i.size() << ", ";
    stream << "#y = " << this->y.size() << ", ";
    stream << "#u = " << this->u.size() << ", ";
    stream << "#p = " << this->p.size() << ", ";
    stream << endl << endl;

    // Variables in the class hierarchy
    stream << "Variables" << endl;

    // Print the variables
    stream << "{" << endl;
    stream << "  t = " << str(this->t) << endl;
    stream << "  s = " << this->s << endl;
    stream << "  x = " << this->x << endl;
    stream << "  z =  " << this->z << endl;
    stream << "  q =  " << this->q << endl;
    stream << "  i =  " << this->i << endl;
    stream << "  y =  " << this->y << endl;
    stream << "  u =  " << this->u << endl;
    stream << "  p =  " << this->p << endl;
    stream << "}" << endl;

    if (!this->dae.empty()) {
      stream << "Fully-implicit differential-algebraic equations" << endl;
      for (int k=0; k<this->dae.size(); ++k) {
        stream << "0 == " << this->dae[k] << endl;
      }
      stream << endl;
    }

    if (!this->x.empty()) {
      stream << "Differential equations" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        stream << str(der(this->x[k])) << " == " << str(this->ode[k]) << endl;
      }
      stream << endl;
    }

    if (!this->alg.empty()) {
      stream << "Algebraic equations" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        stream << "0 == " << str(this->alg[k]) << endl;
      }
      stream << endl;
    }

    if (!this->q.empty()) {
      stream << "Quadrature equations" << endl;
      for (int k=0; k<this->q.size(); ++k) {
        stream << str(der(this->q[k])) << " == " << str(this->quad[k]) << endl;
      }
      stream << endl;
    }

    if (!this->init.empty()) {
      stream << "Initial equations" << endl;
      for (int k=0; k<this->init.size(); ++k) {
        stream << "0 == " << str(this->init[k]) << endl;
      }
      stream << endl;
    }

    if (!this->i.empty()) {
      stream << "Intermediate variables" << endl;
      for (int i=0; i<this->i.size(); ++i)
        stream << this->i[i] << " == " << str(this->idef[i]) << endl;
      stream << endl;
    }

    if (!this->y.empty()) {
      stream << "Output variables" << endl;
      for (int i=0; i<this->y.size(); ++i)
        stream << this->y[i] << " == " << str(this->ydef[i]) << endl;
      stream << endl;
    }

    if (!this->mterm.empty()) {
      stream << "Mayer objective terms" << endl;
      for (int i=0; i<this->mterm.size(); ++i)
        stream << this->mterm[i] << endl;
      stream << endl;
    }

    if (!this->lterm.empty()) {
      stream << "Lagrange objective terms" << endl;
      for (int i=0; i<this->lterm.size(); ++i)
        stream << this->lterm[i] << endl;
      stream << endl;
    }

    // Constraint functions
    stream << "Time horizon" << endl;
    stream << "t0 = " << this->t0 << endl;
    stream << "tf = " << this->tf << endl;
    stream << "tp = " << this->tp << endl;
    if (trailing_newline) stream << endl;
  }

  void SymbolicOCP::eliminate_lterm() {
    // For every integral term in the objective function
    for (int i=0; i<this->lterm.size(); ++i) {

      // Give a name to the quadrature state
      stringstream q_name;
      q_name << "q_" << i;

      // Create a new quadrature state
      Variable qv(q_name.str());

      // Set attributes
      qv.variability = CONTINUOUS;
      qv.causality = INTERNAL;
      qv.start = 0.0;
      if (tf==tf) qv.nominal = this->tf; // if not not-a-number

      // Add to the list of variables
      addVariable(q_name.str(), qv);

      // Add to the quadrature states
      this->q.push_back(qv.v);

      // Add the Lagrange term to the list of quadratures
      this->quad.push_back(this->lterm[i]);

      // Add to the list of Mayer terms
      this->mterm.push_back(qv.v);
    }

    // Remove the Lagrange terms
    this->lterm.clear();
  }

  void SymbolicOCP::eliminate_quad() {
    // Move all the quadratures to the list of differential states
    this->x.insert(this->x.end(), this->q.begin(), this->q.end());
    this->q.clear();
  }

  void SymbolicOCP::scaleVariables() {
    // Gather variables and expressions to replace
    vector<SXElement> v_id, v_rep, ex_rep;
    for (VarMap::iterator it=varmap_.begin(); it!=varmap_.end(); ++it) {
      if (it->second.nominal!=1) {
        Variable& v=it->second;
        casadi_assert(v.nominal!=0);
        v.min /= v.nominal;
        v.max /= v.nominal;
        v.start /= v.nominal;
        v.derivativeStart /= v.nominal;
        v.initialGuess /= v.nominal;
        v_id.push_back(v.v);
        v_id.push_back(v.d);
        v_rep.push_back(v.v * v.nominal);
        v_rep.push_back(v.d * v.nominal);
      }
    }

    // Quick return if no expressions to substitute
    if (v_id.empty()) return;

    // Collect all expressions to be replaced
    vector<SX> ex;
    ex.push_back(vertcat(this->ode));
    ex.push_back(vertcat(this->dae));
    ex.push_back(vertcat(this->alg));
    ex.push_back(vertcat(this->quad));
    ex.push_back(vertcat(this->idef));
    ex.push_back(vertcat(this->ydef));
    ex.push_back(vertcat(this->init));
    ex.push_back(vertcat(this->mterm));
    ex.push_back(vertcat(this->lterm));

    // Substitute all at once (more efficient since they may have common subexpressions)
    ex = substitute(ex, vector<SX>(1, v_id), vector<SX>(1, v_rep));

    // Get the modified expressions
    vector<SX>::const_iterator it=ex.begin();
    this->ode = vertsplit(*it++ / nominal(vertcat(this->x)));
    this->dae = vertsplit(*it++);
    this->alg = vertsplit(*it++);
    this->quad = vertsplit(*it++ / nominal(vertcat(this->q)));
    this->idef = vertsplit(*it++ / nominal(vertcat(this->i)));
    this->ydef = vertsplit(*it++ / nominal(vertcat(this->y)));
    this->init = vertsplit(*it++);
    this->mterm = vertsplit(*it++);
    this->lterm = vertsplit(*it++);
    casadi_assert(it==ex.end());

    // Save the substituted expressions
    vector<SXElement>::iterator ex_rep_it = ex_rep.begin();
    for (VarMap::iterator it=varmap_.begin(); it!=varmap_.end(); ++it) {
      it->second.nominal=1;
    }
    casadi_assert(ex_rep_it==ex_rep.end());
  }

  void SymbolicOCP::sort_i() {
    // Quick return if no intermediates
    if (this->i.empty()) return;

    // Find out which intermediates depends on which other
    SXFunction f(vertcat(this->i), vertcat(this->i) - vertcat(this->idef));
    f.init();
    Sparsity sp = f.jacSparsity();
    casadi_assert(sp.isSquare());

    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.dulmageMendelsohn(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    vector<SX> idefnew(this->i.size()), inew(this->i.size());
    for (int i=0; i<colperm.size(); ++i) {
      // Permute equations
      idefnew[i] = this->idef[colperm[i]];

      // Permute variables
      inew[i] = this->i[colperm[i]];
    }
    this->idef = idefnew;
    this->i = inew;
  }

  void SymbolicOCP::split_i() {
    // Quick return if no intermediates
    if (this->i.empty()) return;

    // Begin by sorting the intermediate
    sort_i();

    // Sort the equations by causality
    vector<SX> ex;
    substituteInPlace(this->i, this->idef, ex);

    // Make sure that the interdependencies have been properly eliminated
    casadi_assert(!dependsOn(vertcat(this->idef), vertcat(this->i)));
  }

  void SymbolicOCP::eliminate_i() {
    // Quick return if possible
    if (this->i.empty()) return;

    // Begin by sorting the intermediate
    sort_i();

    // Collect all expressions to be replaced
    vector<SX> ex;
    ex.push_back(vertcat(this->dae));
    ex.push_back(vertcat(this->ode));
    ex.push_back(vertcat(this->alg));
    ex.push_back(vertcat(this->quad));
    ex.push_back(vertcat(this->ydef));
    ex.push_back(vertcat(this->init));
    ex.push_back(vertcat(this->mterm));
    ex.push_back(vertcat(this->lterm));

    // Substitute all at once (since they may have common subexpressions)
    substituteInPlace(this->i, this->idef, ex);

    // Get the modified expressions
    vector<SX>::const_iterator it=ex.begin();
    this->dae = vertsplit(*it++);
    this->ode = vertsplit(*it++);
    this->alg = vertsplit(*it++);
    this->quad = vertsplit(*it++);
    this->ydef = vertsplit(*it++);
    this->init = vertsplit(*it++);
    this->mterm = vertsplit(*it++);
    this->lterm = vertsplit(*it++);
    casadi_assert(it==ex.end());
  }

  void SymbolicOCP::scaleEquations() {
    casadi_error("SymbolicOCP::scaleEquations broken");
#if 0
    cout << "Scaling equations ..." << endl;
    double time1 = clock();

    // Variables
    enum Variables {T, X, XDOT, Z, P, U, NUM_VAR};
    vector<SX > v(NUM_VAR); // all variables
    v[T] = this->t;
    v[X] = this->x;
    v[XDOT] = der(this->x); // BUG!!!
    v[Z] = this->z;
    v[P] = this->p;
    v[U] = this->u;

    // Create the jacobian of the implicit equations with respect to [x, z, p, u]
    SX xz;
    xz.append(v[X]);
    xz.append(v[Z]);
    xz.append(v[P]);
    xz.append(v[U]);
    SXFunction fcn = SXFunction(xz, this->ode);
    SXFunction J(v, fcn.jac());

    // Evaluate the Jacobian in the starting point
    J.init();
    J.setInput(0.0, T);
    J.setInput(start(this->x, true), X);
    J.input(XDOT).setAll(0.0);
    J.setInput(start(this->z, true), Z);
    J.setInput(start(this->p, true), P);
    J.setInput(start(this->u, true), U);
    J.evaluate();

    // Get the maximum of every row
    Matrix<double> &J0 = J.output();
    vector<double> scale(J0.size1(), 0.0); // scaling factors
    for (int cc=0; cc<J0.size2(); ++cc) {
      // Loop over non-zero entries of the column
      for (int el=J0.colind(cc); el<J0.colind(cc+1); ++el) {
        // Row
        int rr=J0.row(el);

        // The scaling factor is the maximum norm, ignoring not-a-number entries
        if (!isnan(J0.at(el))) {
          scale[rr] = std::max(scale[rr], fabs(J0.at(el)));
        }
      }
    }

    // Make sure nonzero factor found
    for (int rr=0; rr<J0.size1(); ++rr) {
      if (scale[rr]==0) {
        cout << "Warning: Could not generate a scaling factor for equation " << rr;
        scale[rr]=1.;
      }
    }

    // Scale the equations
    this->ode /= scale;

    double time2 = clock();
    double dt = (time2-time1)/CLOCKS_PER_SEC;
    cout << "... equation scaling complete after " << dt << " seconds." << endl;
#endif
  }

  void SymbolicOCP::sort_dae() {
    // Quick return if no differential states
    if (this->x.empty()) return;

    // Find out which differential equation depends on which differential state
    SXFunction f(vertcat(this->sdot), vertcat(this->dae));
    f.init();
    Sparsity sp = f.jacSparsity();
    casadi_assert(sp.isSquare());

    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.dulmageMendelsohn(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    vector<SX> daenew(this->s.size()), snew(this->s.size()), sdotnew(this->s.size());
    for (int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      daenew[i] = this->dae[rowperm[i]];

      // Permute variables
      snew[i] = this->s[colperm[i]];
      sdotnew[i] = this->sdot[colperm[i]];
    }
    this->dae = daenew;
    this->s = snew;
    this->sdot = sdotnew;
  }

  void SymbolicOCP::sort_alg() {
    // Quick return if no algebraic states
    if (this->z.empty()) return;

    // Find out which algebraic equation depends on which algebraic state
    SXFunction f(vertcat(this->z), vertcat(this->alg));
    f.init();
    Sparsity sp = f.jacSparsity();
    casadi_assert(sp.isSquare());

    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.dulmageMendelsohn(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    vector<SX> algnew(this->z.size()), znew(this->z.size());
    for (int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      algnew[i] = this->alg[rowperm[i]];

      // Permute variables
      znew[i] = this->z[colperm[i]];
    }
    this->alg = algnew;
    this->z = znew;
  }

  void SymbolicOCP::makeSemiExplicit() {
    // Only works if there are no i
    eliminate_i();

    // Separate the algebraic variables and equations
    split_dae();

    // Quick return if there are no implicitly defined states
    if (this->s.empty()) return;

    // Write the ODE as a function of the state derivatives
    SXFunction f(vertcat(this->sdot), vertcat(this->dae));
    f.init();

    // Get the sparsity of the Jacobian which can be used to determine which
    // variable can be calculated from which other
    Sparsity sp = f.jacSparsity();
    casadi_assert(sp.isSquare());

    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    int nb = sp.dulmageMendelsohn(rowperm, colperm, rowblock, colblock,
                                  coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    vector<SX> daenew(this->s.size()), snew(this->s.size()), sdotnew(this->s.size());
    for (int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      daenew[i] = this->dae[rowperm[i]];

      // Permute variables
      snew[i] = this->s[colperm[i]];
      sdotnew[i] = this->sdot[colperm[i]];
    }
    this->dae = daenew;
    this->s = snew;
    this->sdot = sdotnew;

    // Now write the sorted ODE as a function of the state derivatives
    f = SXFunction(vertcat(this->sdot), vertcat(this->dae));
    f.init();

    // Get the Jacobian
    SX J = f.jac();

    // Explicit ODE
    vector<SX> new_ode;

    // Loop over blocks
    for (int b=0; b<nb; ++b) {

      // Block size
      int bs = rowblock[b+1] - rowblock[b];

      // Get variables in the block
      vector<SX> xb(this->s.begin()+colblock[b], this->s.begin()+colblock[b+1]);
      vector<SX> xdotb(this->sdot.begin()+colblock[b], this->sdot.begin()+colblock[b+1]);

      // Get equations in the block
      vector<SX> fb(this->dae.begin()+rowblock[b], this->dae.begin()+rowblock[b+1]);

      // Get local Jacobian
      SX Jb = J(Slice(rowblock[b], rowblock[b+1]), Slice(colblock[b], colblock[b+1]));

      // If Jb depends on xb, then the state derivative does not enter linearly
      // in the ODE and we cannot solve for the state derivative
      casadi_assert_message(!dependsOn(Jb, vertcat(xdotb)),
                            "Cannot find an explicit expression for variable(s) " << xb);

      // Divide fb into a part which depends on vb and a part which doesn't according to
      // "fb == mul(Jb, vb) + fb_res"
      vector<SX> fb_res = substitute(fb, xdotb, vector<SX>(xdotb.size(), 0));
      vector<SX> fb_exp;

      // Solve for vb
      if (bs <= 3) {
        // Calculate inverse and multiply for very small matrices
        fb_exp = vertsplit(mul(inv(Jb), -vertcat(fb_res)));
      } else {
        // QR factorization
        fb_exp = vertsplit(solve(Jb, -vertcat(fb_res)));
      }

      // Add to explicitly determined equations and variables
      new_ode.insert(new_ode.end(), fb_exp.begin(), fb_exp.end());
    }

    // Eliminate inter-dependencies
    vector<SX> ex;
    substituteInPlace(this->sdot, new_ode, ex, false);

    // Add to explicit differential states and ODE
    this->x.insert(this->x.end(), this->s.begin(), this->s.end());
    this->ode.insert(this->ode.end(), new_ode.begin(), new_ode.end());
    this->dae.clear();
    this->s.clear();
    this->sdot.clear();
  }

  void SymbolicOCP::eliminate_alg() {
    // Only works if there are no i
    eliminate_i();

    // Quick return if there are no algebraic states
    if (this->z.empty()) return;

    // Write the algebraic equations as a function of the algebraic states
    SXFunction f(vertcat(this->z), vertcat(this->alg));
    f.init();

    // Get the sparsity of the Jacobian which can be used to determine which
    // variable can be calculated from which other
    Sparsity sp = f.jacSparsity();
    casadi_assert(sp.isSquare());

    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    int nb = sp.dulmageMendelsohn(rowperm, colperm, rowblock, colblock,
                                  coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    vector<SX> algnew(this->z.size()), znew(this->z.size());
    for (int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      algnew[i] = this->alg[rowperm[i]];

      // Permute variables
      znew[i] = this->z[colperm[i]];
    }
    this->alg = algnew;
    this->z = znew;

    // Rewrite the sorted algebraic equations as a function of the algebraic states
    f = SXFunction(vertcat(this->z), vertcat(this->alg));
    f.init();

    // Get the Jacobian
    SX J = f.jac();
    casadi_assert(J.isSquare());

    // Variables where we have found an explicit expression and where we haven't
    vector<SX> z_exp, z_imp;

    // Explicit and implicit equations
    vector<SX> f_exp, f_imp;

    // Loop over blocks
    for (int b=0; b<nb; ++b) {

      // Block size
      int bs = rowblock[b+1] - rowblock[b];

      // Get local variables
      vector<SX> zb(this->z.begin()+colblock[b], this->z.begin()+colblock[b+1]);

      // Get local equations
      vector<SX> fb(this->alg.begin()+rowblock[b], this->alg.begin()+rowblock[b+1]);

      // Get local Jacobian
      SX Jb = J(Slice(rowblock[b], rowblock[b+1]), Slice(colblock[b], colblock[b+1]));

      // If Jb depends on zb, then we cannot (currently) solve for it explicitly
      if (dependsOn(Jb, vertcat(zb))) {

        // Add the equations to the new list of algebraic equations
        f_imp.insert(f_imp.end(), fb.begin(), fb.end());

        // ... and the variables accordingly
        z_imp.insert(z_imp.end(), zb.begin(), zb.end());

      } else { // The variables that we wish to determine enter linearly

        // Divide fb into a part which depends on vb and a part which doesn't
        // according to "fb == mul(Jb, vb) + fb_res"
        vector<SX> fb_res = substitute(fb, zb, vector<SX>(zb.size(), 0));

        // Solve for vb
        vector<SX> fb_exp;
        if (bs <= 3) {
          // Calculate inverse and multiply for very small matrices
          fb_exp = vertsplit(mul(inv(Jb), -vertcat(fb_res)));
        } else {
          // QR factorization
          fb_exp = vertsplit(solve(Jb, -vertcat(fb_res)));
        }

        // Add to explicitly determined equations and variables
        z_exp.insert(z_exp.end(), zb.begin(), zb.end());
        f_exp.insert(f_exp.end(), fb_exp.begin(), fb_exp.end());
      }
    }

    // Eliminate inter-dependencies in fb_exp
    vector<SX> ex;
    substituteInPlace(z_exp, f_exp, ex, false);

    // Add to the beginning of the dependent variables
    // (since the other dependent variable might depend on them)
    this->i.insert(this->i.begin(), z_exp.begin(), z_exp.end());
    this->idef.insert(this->idef.begin(), f_exp.begin(), f_exp.end());

    // Save new algebraic equations
    this->z = z_imp;
    this->alg = f_imp;

    // Eliminate new dependent variables from the other equations
    eliminate_i();
  }

  void SymbolicOCP::makeExplicit() {
    // Only works if there are no i
    eliminate_i();

    // Start by transforming to semi-explicit form
    makeSemiExplicit();

    // Then eliminate the algebraic variables
    eliminate_alg();

    // Error if still algebraic variables
    casadi_assert_message(this->z.empty(), "Failed to eliminate algebraic variables");
  }

  const Variable& SymbolicOCP::variable(const std::string& name) const {
    return const_cast<SymbolicOCP*>(this)->variable(name);
  }

  Variable& SymbolicOCP::variable(const std::string& name) {
    // Find the variable
    VarMap::iterator it = varmap_.find(name);
    if (it==varmap_.end()) {
      casadi_error("No such variable: \"" << name << "\".");
    }

    // Return the variable
    return it->second;
  }

  void SymbolicOCP::addVariable(const std::string& name, const Variable& var) {
    // Try to find the component
    if (varmap_.find(name)!=varmap_.end()) {
      stringstream ss;
      casadi_error("Variable \"" << name << "\" has already been added.");
    }

    // Add to the map of all variables
    varmap_[name] = var;
  }

  SX SymbolicOCP::addVariable(const std::string& name) {
    Variable v(name);
    addVariable(name, v);
    return v.v;
  }

  SX SymbolicOCP::add_x(const std::string& name) {
    SX new_x = addVariable(name);
    this->x.push_back(new_x);
    return new_x;
  }

  std::pair<SX, SX> SymbolicOCP::add_s(const std::string& name) {
    Variable v(name);
    addVariable(name, v);
    this->s.push_back(v.v);
    this->sdot.push_back(v.d);
    return std::pair<SX, SX>(v.v, v.d);
  }

  SX SymbolicOCP::add_z(const std::string& name) {
    SX new_z = addVariable(name);
    this->z.push_back(new_z);
    return new_z;
  }

  SX SymbolicOCP::add_p(const std::string& name) {
    SX new_p = addVariable(name);
    this->p.push_back(new_p);
    return new_p;
  }

  SX SymbolicOCP::add_u(const std::string& name) {
    SX new_u = addVariable(name);
    this->u.push_back(new_u);
    return new_u;
  }

  void SymbolicOCP::add_ode(const SX& new_ode) {
    this->ode.push_back(new_ode);
  }

  void SymbolicOCP::add_dae(const SX& new_dae) {
    this->dae.push_back(new_dae);
  }

  void SymbolicOCP::add_alg(const SX& new_alg) {
    this->alg.push_back(new_alg);
  }

  void SymbolicOCP::sanityCheck() const {
    // Time
    casadi_assert_message(this->t.isSymbolic(), "Non-symbolic time t");
    casadi_assert_message(this->t.isScalar(), "Non-scalar time t");

    // Differential states
    casadi_assert_message(this->x.size()==this->ode.size(),
                          "x and ode have different lengths");
    for (int i=0; i<this->x.size(); ++i) {
      casadi_assert_message(this->x[i].shape()==this->ode[i].shape(),
                            "ode has wrong dimensions");
      casadi_assert_message(this->x[i].isSymbolic(), "Non-symbolic state x");
    }

    // DAE
    casadi_assert_message(this->s.size()==this->sdot.size(),
                          "s and sdot have different lengths");
    casadi_assert_message(this->s.size()==this->dae.size(),
                          "s and dae have different lengths");
    for (int i=0; i<this->x.size(); ++i) {
      casadi_assert_message(this->s[i].isSymbolic(), "Non-symbolic state s");
      casadi_assert_message(this->s[i].shape()==this->sdot[i].shape(),
                            "sdot has wrong dimensions");
      casadi_assert_message(this->s[i].shape()==this->dae[i].shape(),
                            "dae has wrong dimensions");
    }

    // Algebraic variables/equations
    casadi_assert_message(this->z.size()==this->alg.size(),
                          "z and alg have different lengths");
    for (int i=0; i<this->z.size(); ++i) {
      casadi_assert_message(this->z[i].isSymbolic(), "Non-symbolic algebraic variable z");
      casadi_assert_message(this->z[i].shape()==this->alg[i].shape(),
                            "alg has wrong dimensions");
    }

    // Quadrature states/equations
    casadi_assert_message(this->q.size()==this->quad.size(),
                          "q and quad have different lengths");
    for (int i=0; i<this->q.size(); ++i) {
      casadi_assert_message(this->q[i].isSymbolic(), "Non-symbolic quadrature state q");
      casadi_assert_message(this->q[i].shape()==this->quad[i].shape(),
                            "quad has wrong dimensions");
    }

    // Intermediate variables
    casadi_assert_message(this->i.size()==this->idef.size(),
                          "i and idef have different lengths");
    for (int i=0; i<this->i.size(); ++i) {
      casadi_assert_message(this->i[i].isSymbolic(), "Non-symbolic intermediate variable i");
      casadi_assert_message(this->i[i].shape()==this->idef[i].shape(),
                            "idef has wrong dimensions");
    }

    // Output equations
    casadi_assert_message(this->y.size()==this->ydef.size(),
                          "y and ydef have different lengths");
    for (int i=0; i<this->i.size(); ++i) {
      casadi_assert_message(this->y[i].isSymbolic(), "Non-symbolic output y");
      casadi_assert_message(this->y[i].shape()==this->ydef[i].shape(),
                            "ydef has wrong dimensions");
    }

    // Control
    for (int i=0; i<this->u.size(); ++i) {
      casadi_assert_message(this->u[i].isSymbolic(), "Non-symbolic control u");
    }

    // Parameter
    for (int i=0; i<this->p.size(); ++i) {
      casadi_assert_message(this->p[i].isSymbolic(), "Non-symbolic parameter p");
    }
  }

  std::string SymbolicOCP::qualifiedName(const XmlNode& nn) {
    // Stringstream to assemble name
    stringstream qn;

    for (int i=0; i<nn.size(); ++i) {
      // Add a dot
      if (i!=0) qn << ".";

      // Get the name part
      qn << nn[i].getAttribute("name");

      // Get the index, if any
      if (nn[i].size()>0) {
        int ind;
        nn[i]["exp:ArraySubscripts"]["exp:IndexExpression"]["exp:IntegerLiteral"].getText(ind);
        qn << "[" << ind << "]";
      }
    }

    // Return the name
    return qn.str();
  }

  void SymbolicOCP::generateMuscodDatFile(const std::string& filename,
                                          const Dictionary& mc2_ops) const {
    // Print
    cout << "Generating: " << filename << endl;

    // Create the datfile
    ofstream datfile;
    datfile.open(filename.c_str());
    datfile.precision(numeric_limits<double>::digits10+2);
    datfile << scientific; // This is really only to force a decimal dot,
                           // would be better if it can be avoided

    // Print header
    datfile << "* This function was automatically generated by CasADi" << endl;
    datfile << endl;

    // User set options
    for (Dictionary::const_iterator it=mc2_ops.begin(); it!=mc2_ops.end(); ++it) {
      // Print the name
      datfile << it->first << endl;

      // Get the value
      const GenericType& val = it->second;

      // Print value
      if (val.isInt()) {
        datfile << static_cast<int>(val) << endl;
      } else if (val.isDouble()) {
        datfile << static_cast<double>(val) << endl;
      } else if (val.isString()) {
        datfile << string(val) << endl;
      } else if (val.isIntVector()) {
        vector<int> valv = val;
        for (int k=0; k<valv.size(); ++k) {
          datfile << k << ": " << valv[k] << endl;
        }
      } else if (val.isDoubleVector()) {
        vector<double> valv = val;
        for (int k=0; k<valv.size(); ++k) {
          datfile << k << ": " << valv[k] << endl;
        }
      } else if (val.isStringVector()) {
        vector<string> valv = val;
        for (int k=0; k<valv.size(); ++k) {
          datfile << k << ": " << valv[k] << endl;
        }
      }
      datfile << endl;
    }

    // Get the stage duration
    double h = tf-t0;

    // Is the stage duration fixed?
    bool h_fix = !t0_free && !tf_free;

    // Get bounds on the stage duration
    double h_min=h, h_max=h;

    // Set to some dummy variables if stage duration not fixed
    if (!h_fix) {
      casadi_warning("h_min and h_max being set to dummy variables!");
      h_min = 0;
      h_max = numeric_limits<double>::infinity();
    }

    datfile << "* model stage duration start values, scale factors, and bounds" << endl;
    datfile << "h" << endl;
    datfile << "0: " << h << endl;
    datfile << endl;

    datfile << "h_sca" << endl;
    datfile << "0: " << h << endl;
    datfile << endl;

    datfile << "h_min" << endl;
    datfile << "0: " << h_min << endl;
    datfile << endl;

    datfile << "h_max" << endl;
    datfile << "0: " << h_max << endl;
    datfile << endl;

    datfile << "h_fix" << endl;
    datfile << "0: " << h_fix << endl;
    datfile << endl;

    // Parameter properties
    if (!this->p.empty()) {
      datfile << "*  global model parameter start values, scale factors, and bounds" << endl;
      datfile << "p" << endl;
      for (int k=0; k<this->p.size(); ++k) {
        datfile << k << ": " << start(this->p[k]) << endl;
      }
      datfile << endl;

      datfile << "p_sca" << endl;
      for (int k=0; k<this->p.size(); ++k) {
        datfile << k << ": " << nominal(this->p[k]) << endl;
      }
      datfile << endl;

      datfile << "p_min" << endl;
      for (int k=0; k<this->p.size(); ++k) {
        datfile << k << ": " << min(this->p[k]) << endl;
      }
      datfile << endl;

      datfile << "p_max" << endl;
      for (int k=0; k<this->p.size(); ++k) {
        datfile << k << ": " << max(this->p[k]) << endl;
      }
      datfile << endl;

      datfile << "p_fix" << endl;
      for (int k=0; k<this->p.size(); ++k) {
        datfile << k << ": " << (min(this->p[k])==max(this->p[k])) << endl;
      }
      datfile << endl;

      datfile << "p_name" << endl;
      for (int k=0; k<this->p.size(); ++k) {
        datfile << k << ": " << this->p[k].getName() << endl;
      }
      datfile << endl;

      datfile << "p_unit" << endl;
      for (int k=0; k<this->p.size(); ++k) {
        datfile << k << ": " << unit(this->p[k]) << endl;
      }
      datfile << endl;
    }

    // Differential state properties
    if (!this->x.empty()) {
      datfile << "*  differential state start values, scale factors, and bounds" << endl;
      datfile << "sd(*,*)" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        datfile << k << ": " << start(this->x[k]) << endl;
      }
      datfile << endl;

      datfile << "sd_sca(*,*)" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        datfile << k << ": " << nominal(this->x[k]) << endl;
      }
      datfile << endl;

      datfile << "sd_min(*,*)" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        datfile << k << ": " << min(this->x[k]) << endl;
      }
      datfile << endl;

      datfile << "sd_max(*,*)" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        datfile << k << ": " << max(this->x[k]) << endl;
      }
      datfile << endl;

      datfile << "sd_fix(*,*)" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        datfile << k << ": " << (min(this->x[k])==max(this->x[k])) << endl;
      }
      datfile << endl;

      datfile << "xd_name" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        datfile << k << ": " << this->x[k].getName() << endl;
      }
      datfile << endl;

      datfile << "xd_unit" << endl;
      for (int k=0; k<this->x.size(); ++k) {
        datfile << k << ": " << unit(this->x[k]) << endl;
      }
      datfile << endl;
    }

    // Algebraic state properties
    if (!this->z.empty()) {
      datfile << "*  algebraic state start values, scale factors, and bounds" << endl;
      datfile << "sa(*,*)" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        datfile << k << ": " << start(this->z[k]) << endl;
      }
      datfile << endl;

      datfile << "sa_sca(*,*)" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        datfile << k << ": " << nominal(this->z[k]) << endl;
      }
      datfile << endl;

      datfile << "sa_min(*,*)" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        datfile << k << ": " << min(this->z[k]) << endl;
      }
      datfile << endl;

      datfile << "sa_max(*,*)" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        datfile << k << ": " << max(this->z[k]) << endl;
      }
      datfile << endl;

      datfile << "sa_fix(*,*)" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        datfile << k << ": " << (min(this->z[k])==max(this->z[k])) << endl;
      }
      datfile << endl;

      datfile << "xa_name" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        datfile << k << ": " << this->z[k].getName() << endl;
      }
      datfile << endl;

      datfile << "xa_unit" << endl;
      for (int k=0; k<this->z.size(); ++k) {
        datfile << k << ": " << unit(this->z[k]) << endl;
      }
      datfile << endl;
    }

    // Control properties
    if (!this->u.empty()) {
      datfile << "* control start values, scale factors, and bounds" << endl;
      datfile << "u(*,*)" << endl;
      for (int k=0; k<this->u.size(); ++k) {
        datfile << k << ": " << start(this->u[k]) << endl;
      }
      datfile << endl;

      datfile << "u_sca(*,*)" << endl;
      for (int k=0; k<this->u.size(); ++k) {
        datfile << k << ": " << nominal(this->u[k]) << endl;
      }
      datfile << endl;

      datfile << "u_min(*,*)" << endl;
      for (int k=0; k<this->u.size(); ++k) {
        datfile << k << ": " << min(this->u[k]) << endl;
      }
      datfile << endl;

      datfile << "u_max(*,*)" << endl;
      for (int k=0; k<this->u.size(); ++k) {
        datfile << k << ": " << max(this->u[k]) << endl;
      }
      datfile << endl;

      datfile << "u_fix(*,*)" << endl;
      for (int k=0; k<this->u.size(); ++k) {
        datfile << k << ": " << (min(this->u[k])==max(this->u[k])) << endl;
      }
      datfile << endl;

      datfile << "u_name" << endl;
      for (int k=0; k<this->u.size(); ++k) {
        datfile << k << ": " << this->u[k].getName() << endl;
      }
      datfile << endl;

      datfile << "u_unit" << endl;
      for (int k=0; k<this->u.size(); ++k) {
        datfile << k << ": " << unit(this->u[k]) << endl;
      }
      datfile << endl;
    }

    // Close the datfile
    datfile.close();
  }

  SX SymbolicOCP::operator()(const std::string& name) const {
    return variable(name).v;
  }

  SX SymbolicOCP::der(const std::string& name) const {
    return variable(name).d;
  }

  SX SymbolicOCP::der(const SX& var) const {
    casadi_assert(var.isVector() && var.isSymbolic());
    SX ret = SX::zeros(var.sparsity());
    for (int i=0; i<ret.nnz(); ++i) {
      ret[i] = der(var.at(i).getName());
    }
    return ret;
  }

  void SymbolicOCP::split_dae() {
    // Only works if there are no i
    eliminate_i();

    // Quick return if no s
    if (this->s.empty()) return;

    // We investigate the interdependencies in sdot -> dae
    vector<SX> f_in;
    f_in.push_back(vertcat(this->sdot));
    SXFunction f(f_in, vertcat(this->dae));
    f.init();

    // Number of s
    int ns = f.input().nnz();
    casadi_assert(f.output().nnz()==ns);

    // Input/output arrays
    bvec_t* f_sdot = reinterpret_cast<bvec_t*>(f.input().ptr());
    bvec_t* f_dae = reinterpret_cast<bvec_t*>(f.output().ptr());

    // First find out which equations depend on sdot
    f.spInit(true);

    // Seed all inputs
    std::fill(f_sdot, f_sdot+ns, bvec_t(1));

    // Propagate to f_dae
    std::fill(f_dae, f_dae+ns, bvec_t(0));
    f.spEvaluate(true);

    // Get the new differential and algebraic equations
    vector<SX> new_dae, new_alg;
    for (int i=0; i<ns; ++i) {
      if (f_dae[i]==bvec_t(1)) {
        new_dae.push_back(this->dae[i]);
      } else {
        casadi_assert(f_dae[i]==bvec_t(0));
        new_alg.push_back(this->dae[i]);
      }
    }

    // Now find out what sdot enter in the equations
    f.spInit(false);

    // Seed all outputs
    std::fill(f_dae, f_dae+ns, bvec_t(1));

    // Propagate to f_sdot
    std::fill(f_sdot, f_sdot+ns, bvec_t(0));
    f.spEvaluate(false);

    // Get the new algebraic variables and new states
    vector<SX> new_s, new_sdot, new_z;
    for (int i=0; i<ns; ++i) {
      if (f_sdot[i]==bvec_t(1)) {
        new_s.push_back(this->s[i]);
        new_sdot.push_back(this->sdot[i]);
      } else {
        casadi_assert(f_sdot[i]==bvec_t(0));
        new_z.push_back(this->s[i]);
      }
    }

    // Make sure split was successful
    casadi_assert(new_dae.size()==new_s.size());

    // Divide up the s and dae
    this->dae = new_dae;
    this->s = new_s;
    this->sdot = new_sdot;
    this->alg.insert(this->alg.end(), new_alg.begin(), new_alg.end());
    this->z.insert(this->z.end(), new_z.begin(), new_z.end());
  }

  std::string SymbolicOCP::unit(const std::string& name) const {
    return variable(name).unit;
  }

  std::string SymbolicOCP::unit(const SX& var) const {
    casadi_assert_message(!var.isVector() && var.isSymbolic(),
                          "SymbolicOCP::unit: Argument must be a symbolic vector");
    if (var.isEmpty()) {
      return "n/a";
    } else {
      string ret = unit(var.at(0).getName());
      for (int i=1; i<var.nnz(); ++i) {
        casadi_assert_message(ret == unit(var.at(i).getName()),
                              "SymbolicOCP::unit: Argument has mixed units");
      }
      return ret;
    }
  }

  void SymbolicOCP::setUnit(const std::string& name, const std::string& val) {
    variable(name).unit = val;
  }

  double SymbolicOCP::nominal(const std::string& name) const {
    return variable(name).nominal;
  }

  void SymbolicOCP::setNominal(const std::string& name, double val) {
    variable(name).nominal = val;
  }

  std::vector<double> SymbolicOCP::nominal(const SX& var) const {
    casadi_assert_message(var.isVector() && var.isSymbolic(),
                          "SymbolicOCP::nominal: Argument must be a symbolic vector");
    std::vector<double> ret(var.nnz());
    for (int i=0; i<ret.size(); ++i) {
      ret[i] = nominal(var.at(i).getName());
    }
    return ret;
  }

  void SymbolicOCP::setNominal(const SX& var, const std::vector<double>& val) {
    casadi_assert_message(var.isVector() && var.isSymbolic(),
                          "SymbolicOCP::nominal: Argument must be a symbolic vector");
    casadi_assert_message(var.nnz()==var.nnz(), "SymbolicOCP::nominal: Dimension mismatch");
    for (int i=0; i<val.size(); ++i) {
      setNominal(var.at(i).getName(), val.at(i));
    }
  }

  std::vector<double> SymbolicOCP::attribute(getAtt f, const SX& var, bool normalized) const {
    casadi_assert_message(var.isVector() && var.isSymbolic(),
                          "SymbolicOCP::attribute: Argument must be a symbolic vector");
    std::vector<double> ret(var.nnz());
    for (int i=0; i<ret.size(); ++i) {
      ret[i] = (this->*f)(var.at(i).getName(), normalized);
    }
    return ret;
  }

  SX SymbolicOCP::attribute(getAttS f, const SX& var) const {
    casadi_assert_message(var.isVector() && var.isSymbolic(),
                          "SymbolicOCP::attribute: Argument must be a symbolic vector");
    SX ret = SX::zeros(var.sparsity());
    for (int i=0; i<ret.nnz(); ++i) {
      ret[i] = (this->*f)(var.at(i).getName());
    }
    return ret;
  }

  void SymbolicOCP::setAttribute(setAtt f, const SX& var, const std::vector<double>& val,
                                 bool normalized) {
    casadi_assert_message(var.isVector() && var.isSymbolic(),
                          "SymbolicOCP::setAttribute: Argument must be a symbolic vector");
    casadi_assert_message(var.nnz()==val.size(), "SymbolicOCP::setAttribute: Dimension mismatch");
    for (int i=0; i<val.size(); ++i) {
      (this->*f)(var.at(i).getName(), val.at(i), normalized);
    }
  }

  void SymbolicOCP::setAttribute(setAttS f, const SX& var, const SX& val) {
    casadi_assert_message(var.isVector() && var.isSymbolic(),
                          "SymbolicOCP::setAttribute: Argument must be a symbolic vector");
    casadi_assert_message(var.sparsity()==val.sparsity(),
                          "SymbolicOCP::setAttribute: Sparsity mismatch");
    for (int i=0; i<val.nnz(); ++i) {
      (this->*f)(var.at(i).getName(), val.at(i));
    }
  }

  double SymbolicOCP::min(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.min / v.nominal : v.min;
  }

  std::vector<double> SymbolicOCP::min(const SX& var, bool normalized) const {
    return attribute(&SymbolicOCP::min, var, normalized);
  }

  void SymbolicOCP::setMin(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.min = normalized ? val*v.nominal : val;
  }

  void SymbolicOCP::setMin(const SX& var, const std::vector<double>& val, bool normalized) {
    setAttribute(&SymbolicOCP::setMin, var, val, normalized);
  }

  double SymbolicOCP::max(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.max / v.nominal : v.max;
  }

  std::vector<double> SymbolicOCP::max(const SX& var, bool normalized) const {
    return attribute(&SymbolicOCP::max, var, normalized);
  }

  void SymbolicOCP::setMax(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.max = normalized ? val*v.nominal : val;
  }

  void SymbolicOCP::setMax(const SX& var, const std::vector<double>& val, bool normalized) {
    setAttribute(&SymbolicOCP::setMax, var, val, normalized);
  }

  double SymbolicOCP::initialGuess(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.initialGuess / v.nominal : v.initialGuess;
  }

  std::vector<double> SymbolicOCP::initialGuess(const SX& var, bool normalized) const {
    return attribute(&SymbolicOCP::initialGuess, var, normalized);
  }

  void SymbolicOCP::setInitialGuess(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.initialGuess = normalized ? val*v.nominal : val;
  }

  void SymbolicOCP::setInitialGuess(const SX& var, const std::vector<double>& val,
                                    bool normalized) {
    setAttribute(&SymbolicOCP::setInitialGuess, var, val, normalized);
  }

  double SymbolicOCP::start(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.start / v.nominal : v.start;
  }

  std::vector<double> SymbolicOCP::start(const SX& var, bool normalized) const {
    return attribute(&SymbolicOCP::start, var, normalized);
  }

  void SymbolicOCP::setStart(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.start = normalized ? val*v.nominal : val;
  }

  void SymbolicOCP::setStart(const SX& var, const std::vector<double>& val, bool normalized) {
    setAttribute(&SymbolicOCP::setStart, var, val, normalized);
  }

  double SymbolicOCP::derivativeStart(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.derivativeStart / v.nominal : v.derivativeStart;
  }

  std::vector<double> SymbolicOCP::derivativeStart(const SX& var, bool normalized) const {
    return attribute(&SymbolicOCP::derivativeStart, var, normalized);
  }

  void SymbolicOCP::setDerivativeStart(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.derivativeStart = normalized ? val*v.nominal : val;
  }

  void SymbolicOCP::setDerivativeStart(const SX& var, const std::vector<double>& val,
                                       bool normalized) {
    setAttribute(&SymbolicOCP::setDerivativeStart, var, val, normalized);
  }

  void SymbolicOCP::generateFunctionHeader(std::ostream &stream, const std::string& fname,
                                           bool fwd, bool adj, bool foa) {
    stream << "void " << fname << "_nwork(int *ni, int *nr);" << endl;
    stream << "int " << fname
           << "(const double* const* arg, double* const* res, int* iw, double* w);" << endl;

    // Codegen derivative information
    if (fwd) generateFunctionHeader(stream, fname+"_fwd");
    if (adj) generateFunctionHeader(stream, fname+"_adj");
    if (foa) generateFunctionHeader(stream, fname+"_foa");
  }

  void SymbolicOCP::generateHeader(const std::string& filename, const std::string& prefix) {
    // Create header file
    ofstream s;
    s.open(filename.c_str());

    // Print header
    s << "/* This file was automatically generated by CasADi */" << endl << endl;

    // Include guards
    s << "#ifndef " << prefix << "OCP_HEADER_FILE" << endl;
    s << "#define "<< prefix << "OCP_HEADER_FILE" << endl << endl;

    // C linkage
    s << "#ifdef __cplusplus" << endl;
    s << "extern \"C\" {" << endl;
    s << "#endif" << endl << endl;

    // Typedef input enum corresponding to generated format
    s << "/* Input convension */" << endl;
    s << "typedef enum {"
      << "OCP_T, "
      << "OCP_X, "
      << "OCP_S, "
      << "OCP_SDOT, "
      << "OCP_Z, "
      << "OCP_U, "
      << "OCP_Q, "
      << "OCP_I, "
      << "OCP_Y, "
      << "OCP_P, "
      << "OCP_NUM_IN} " << prefix << "ocp_input_t;" << endl << endl;

    // Typedef input enum corresponding to generated format
    s << "/* Output convension */" << endl;
    s << "typedef enum {"
      << "OCP_ODE, "
      << "OCP_DAE, "
      << "OCP_ALG, "
      << "OCP_QUAD, "
      << "OCP_IDEF, "
      << "OCP_YDEF, "
      << "OCP_NUM_OUT} " << prefix << "ocp_output_t;" << endl << endl;

    // Input dimensions and offset
    s << "/* Input dimensions and offsets */" << endl
      << "void " << prefix
      << "input_dims(const int **dims, const int **offset, const int **inv_offset);" << endl
      << endl;

    // Output dimensions and offset
    s << "/* Output dimensions and offsets */" << endl
      << "void " << prefix
      << "output_dims(const int **dims, const int **offset, const int **inv_offset);" << endl
      << endl;

    s << "/* Function declarations */" << endl;
    generateFunctionHeader(s, prefix+"eval_ode");
    generateFunctionHeader(s, prefix+"eval_dae");
    generateFunctionHeader(s, prefix+"eval_alg");
    generateFunctionHeader(s, prefix+"eval_quad");
    generateFunctionHeader(s, prefix+"eval_idef");
    generateFunctionHeader(s, prefix+"eval_ydef");
    generateFunctionHeader(s, prefix+"eval", true, true, true);
    s << endl;

    s << "/* Jacobian of all outputs w.r.t. all inputs */" << endl;
    generateFunctionHeader(s, prefix+"eval_jac");
    s << "void " << prefix <<
      "jac_sparsity(int *nrow, int *ncol, const int **colind, const int **row);" << endl;
    s << endl;

    s << "/* Hessian of all outputs w.r.t. all inputs */" << endl;
    generateFunctionHeader(s, prefix+"eval_hes");
    s << "void " << prefix <<
      "hes_sparsity(int *nrow, int *ncol, const int **colind, const int **row);" << endl;
    s << endl;

    // C linkage
    s << "#ifdef __cplusplus" << endl;
    s << "} /* extern \"C\" */" << endl;
    s << "#endif" << endl << endl;

    // Include guards
    s << "#endif /* " << prefix << "OCP_HEADER_FILE */" << endl << endl;

    s.close();
  }

  void SymbolicOCP::generateFunction(std::ostream &stream, const std::string& fname,
                                     const std::vector<SX>& f_in,
                                     const std::vector<SX>& f_out,
                                     CodeGenerator& gen,
                                     bool fwd, bool adj, bool foa) {
    SXFunction f(f_in, f_out);
    f.setOption("name", fname);
    f.init();
    f.generateFunction(stream, fname, "double", gen);

    // No work vector needed for now (SXFunction)
    stream << "void " << fname << "_nwork(int *ni, int *nr) {" << endl;
    stream << "  if (ni) *ni = 0;" << endl;
    stream << "  if (nr) *nr = 0;" << endl;
    stream << "}" << endl << endl;

    // Forward mode directional derivative
    if (fwd) {
      SXFunction f_fwd = shared_cast<SXFunction>(f.derForward(1));
      generateFunction(stream, fname+"_fwd",
                       f_fwd.inputExpr(), f_fwd.outputExpr(), gen);
    }

    // Reverse mode mode directional derivative
    if (adj || foa) {
      SXFunction f_adj = shared_cast<SXFunction>(f.derReverse(1));
      if (adj) {
        generateFunction(stream, fname+"_adj",
                         f_adj.inputExpr(), f_adj.outputExpr(), gen);
      }
      // Forward-over-reverse mode directional derivative
      if (foa) {
        SXFunction f_foa = shared_cast<SXFunction>(f_adj.derForward(1));
        generateFunction(stream, fname+"_foa",
                         f_foa.inputExpr(), f_foa.outputExpr(), gen);
      }
    }
  }

  void SymbolicOCP::generateCode(const std::string& filename,
                                 const Dictionary& options) {
    // Default options
    string prefix = "";
    string include = "";

    // Read options
    for (Dictionary::const_iterator it=options.begin(); it!=options.end(); ++it) {
      if (it->first=="prefix") {
        prefix = it->second.toString();
      } else if (it->first=="include") {
        include = it->second.toString();
      } else {
        casadi_error("Unrecongnized option: " << it->first);
      }
    }

    // Create file
    ofstream s;
    s.open(filename.c_str());
    s.precision(std::numeric_limits<double>::digits10+2);
    s << std::scientific;

    // Print header
    s << "/* This file was automatically generated by CasADi */" << endl << endl;

    // C linkage
    s << "#ifdef __cplusplus" << endl;
    s << "extern \"C\" {" << endl;
    s << "#endif" << endl << endl;

    // Create a code generator object
    CodeGenerator gen;
    if (!include.empty()) {
      gen.addInclude(include, true);
    }

    // All inputs
    vector<SX> v_in;
    v_in.push_back(this->t);
    v_in.push_back(vertcat(this->x));
    v_in.push_back(vertcat(this->s));
    v_in.push_back(vertcat(this->sdot));
    v_in.push_back(vertcat(this->z));
    v_in.push_back(vertcat(this->u));
    v_in.push_back(vertcat(this->q));
    v_in.push_back(vertcat(this->i));
    v_in.push_back(vertcat(this->y));
    v_in.push_back(vertcat(this->p));

    // Input dimensions
    vector<int> dims(v_in.size());
    for (int i=0; i<v_in.size(); ++i) {
      dims[i] = v_in[i].nnz();
    }
    int dims_ind = gen.getConstant(dims, true);

    // Corresponding offsets
    dims.insert(dims.begin(), 0);
    vector<int> inv_offset;
    for (int i=0; i<v_in.size(); ++i) {
      dims[i+1] += dims[i]; // cumsum
      inv_offset.resize(dims[i+1], i);
    }
    int offset_ind = gen.getConstant(dims, true);
    int inv_offset_ind = gen.getConstant(inv_offset, true);
    gen.function_
      << "void " << prefix
      << "input_dims(const int **dims, const int **offset, const int **inv_offset) {" << endl
      << "  if (dims) *dims = s" << dims_ind << ";" << endl
      << "  if (offset) *offset = s" << offset_ind << ";" << endl
      << "  if (inv_offset) *inv_offset = s" << inv_offset_ind << ";" << endl
      << "}" << endl << endl;

    // All outputs
    vector<SX> v_out;
    v_out.push_back(vertcat(this->ode));
    v_out.push_back(vertcat(this->dae));
    v_out.push_back(vertcat(this->alg));
    v_out.push_back(vertcat(this->quad));
    v_out.push_back(vertcat(this->idef));
    v_out.push_back(vertcat(this->ydef));

    // Output dimensions
    dims.resize(v_out.size());
    for (int i=0; i<v_out.size(); ++i) {
      dims[i] = v_out[i].nnz();
    }
    dims_ind = gen.getConstant(dims, true);

    // Corresponding offsets
    dims.insert(dims.begin(), 0);
    inv_offset.clear();
    for (int i=0; i<v_out.size(); ++i) {
      dims[i+1] += dims[i]; // cumsum
      inv_offset.resize(dims[i+1], i);
    }
    offset_ind = gen.getConstant(dims, true);
    inv_offset_ind = gen.getConstant(inv_offset, true);
    gen.function_
      << "void " << prefix
      << "output_dims(const int **dims, const int **offset, const int **inv_offset) {" << endl
      << "  if (dims) *dims = s" << dims_ind << ";" << endl
      << "  if (offset) *offset = s" << offset_ind << ";" << endl
      << "  if (inv_offset) *inv_offset = s" << inv_offset_ind << ";" << endl
      << "}" << endl << endl;

    // Basic functions individually
    generateFunction(gen.function_, prefix+"eval_ode", v_in,
                     vector<SX>(1, vertcat(this->ode)), gen);
    generateFunction(gen.function_, prefix+"eval_dae", v_in,
                     vector<SX>(1, vertcat(this->dae)), gen);
    generateFunction(gen.function_, prefix+"eval_alg", v_in,
                     vector<SX>(1, vertcat(this->alg)), gen);
    generateFunction(gen.function_, prefix+"eval_quad", v_in,
                     vector<SX>(1, vertcat(this->quad)), gen);
    generateFunction(gen.function_, prefix+"eval_idef", v_in,
                     vector<SX>(1, vertcat(this->idef)), gen);
    generateFunction(gen.function_, prefix+"eval_ydef", v_in,
                     vector<SX>(1, vertcat(this->ydef)), gen);

    // All functions at once, with derivatives
    generateFunction(gen.function_, prefix+"eval", v_in, v_out, gen, true, true, true);

    // Jacobian of all input w.r.t. all outputs
    SX v_in_all = vertcat(v_in);
    SX v_out_all = vertcat(v_out);
    SX J = jacobian(v_out_all, v_in_all);

    // Codegen it
    generateFunction(gen.function_, prefix+"eval_jac", v_in, vector<SX>(1, J), gen);
    int Jsp_ind = gen.addSparsity(J.sparsity());
    gen.function_
      << "void " << prefix
      << "jac_sparsity(int *nrow, int *ncol, const int **colind, const int **row) {" << endl
      << "  const int *s = s" << Jsp_ind << ";" << endl
      << "  if (nrow) *nrow = s[0];" << endl
      << "  if (ncol) *ncol = s[1];" << endl
      << "  if (colind) *colind = s+2;" << endl
      << "  if (row) *row = s+2+s[1]+1;" << endl
      << "}" << endl << endl;

    // Introduce lagrange multipliers
    vector<SX> lam;
    lam.push_back(SX::sym("lam_ode", vertcat(this->ode).sparsity()));
    lam.push_back(SX::sym("lam_dae", vertcat(this->dae).sparsity()));
    lam.push_back(SX::sym("lam_alg", vertcat(this->alg).sparsity()));
    lam.push_back(SX::sym("lam_quad", vertcat(this->quad).sparsity()));
    lam.push_back(SX::sym("lam_idef", vertcat(this->idef).sparsity()));
    lam.push_back(SX::sym("lam_ydef", vertcat(this->ydef).sparsity()));

    // Jacobian of all input w.r.t. all outputs
    SX lam_all = vertcat(lam);
    SX gamma = inner_prod(v_out_all, lam_all);
    SX H = hessian(gamma, v_in_all);
    H = triu(H); // Upper triangular half
    v_in.insert(v_in.begin(), lam.begin(), lam.end());

    // Codegen it
    generateFunction(gen.function_, prefix+"eval_hes", v_in, vector<SX>(1, H), gen);
    int Hsp_ind = gen.addSparsity(H.sparsity());
    gen.function_
      << "void " << prefix
      << "hes_sparsity(int *nrow, int *ncol, const int **colind, const int **row) {" << endl
      << "  const int *s = s" << Hsp_ind << ";" << endl
      << "  if (nrow) *nrow = s[0];" << endl
      << "  if (ncol) *ncol = s[1];" << endl
      << "  if (colind) *colind = s+2;" << endl
      << "  if (row) *row = s+2+s[1]+1;" << endl
      << "}" << endl << endl;

    // Flush the code generator
    gen.flush(s);

    // C linkage
    s << "#ifdef __cplusplus" << endl;
    s << "} /* extern \"C\" */" << endl;
    s << "#endif" << endl;

    // Finalize
    s << endl;
    s.close();
  }

} // namespace casadi

