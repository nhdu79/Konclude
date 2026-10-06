/*
 *		Copyright (C) 2013-2015, 2019 by the Konclude Developer Team.
 *
 *		This file is part of the reasoning system Konclude.
 *		For details and support, see <http://konclude.com/>.
 *
 *		Konclude is free software: you can redistribute it and/or modify
 *		it under the terms of version 3 of the GNU Lesser General Public
 *		License (LGPLv3) as published by the Free Software Foundation.
 *
 *		Konclude is distributed in the hope that it will be useful,
 *		but WITHOUT ANY WARRANTY; without even the implied warranty of
 *		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *		GNU (Lesser) General Public License for more details.
 *
 *		You should have received a copy of the GNU (Lesser) General Public
 *		License along with Konclude. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "CConfigDependedSubsumptionClassifierFactory.h"


namespace Konclude {

	namespace Reasoner {

		namespace Classifier {


			CConfigDependedSubsumptionClassifierFactory::CConfigDependedSubsumptionClassifierFactory(CReasonerManager *reasonerManager) {
				reasonerMan = reasonerManager;
				mIncrementalKPSetClassClassifier = nullptr;
				mExtractedSaturationClassClassifier = nullptr;
				mSubClassClassifier = nullptr;
				mKPSetClassClassifier = nullptr;
				mDataPropertyClassifier = nullptr;
				mObjectPropertyClassifier = nullptr;
			}


			CConfigDependedSubsumptionClassifierFactory::~CConfigDependedSubsumptionClassifierFactory() {
			}

			CSubsumptionClassifier *CConfigDependedSubsumptionClassifierFactory::createClassifier(CConcreteOntology *ontology, CConfigurationBase *config) {
				CSubsumptionClassifier *classifier = 0;
				bool automatedClassifierSelection = CConfigDataReader::readConfigBoolean(config,"Konclude.Calculation.Classification.AutomatedOptimizedClassifierSelection",true);
				if (automatedClassifierSelection) {

					bool onlyChangedABoxAxioms = ontology->getIncrementalRevisionData()->getAxiomChangeData()->hasOnlyChangedABoxAxioms();
					bool previousClassifiedOntologyVersion = ontology->getIncrementalRevisionData()->getPreviousClassesClassifiedOntology() != nullptr;
					if (previousClassifiedOntologyVersion && onlyChangedABoxAxioms) {
						if (!mIncrementalKPSetClassClassifier) {
							mIncrementalKPSetClassClassifier = new CIncrementalKPSetClassSubsumptionClassifierThread(reasonerMan);
						}
						classifier = mIncrementalKPSetClassClassifier;
					} else if (isClassificationBySaturationCalculationSufficient(ontology,config)) {
						LOG(INFO,"::Konclude::Reasoner::Classifier::Factory",logTr("Ontology '%1' has been sufficiently saturated, extracting data for classification.").arg(ontology->getOntologyName()),this);
						if (!mExtractedSaturationClassClassifier) {
							mExtractedSaturationClassClassifier = new COptimizedClassExtractedSaturationSubsumptionClassifierThread(reasonerMan);
						}
						classifier = mExtractedSaturationClassClassifier;
					} else if (isClassificationBySatisfiableCalculationSufficient(ontology,config)) {
						LOG(INFO,"::Konclude::Reasoner::Classifier::Factory",logTr("Ontology '%1' is deterministic, using satisfiable tests for classification.").arg(ontology->getOntologyName()),this);
						if (!mSubClassClassifier) {
							mSubClassClassifier = new COptimizedSubClassSubsumptionClassifierThread(reasonerMan);
						}
						classifier = mSubClassClassifier;
					}
				}
				if (!classifier) {
					QString classifierName = CConfigDataReader::readConfigString(config,"Konclude.Calculation.Classification.Classifier","Konclude.Calculation.Classification.Classifier.OptimizedKPSetClassClassifier");

					if (classifierName == "Konclude.Calculation.Classification.Classifier.OptimizedSubClassClassifier") {
						if (!mSubClassClassifier) {
							mSubClassClassifier = new COptimizedSubClassSubsumptionClassifierThread(reasonerMan);
						}
						classifier = mSubClassClassifier;
					} else if (classifierName == "Konclude.Calculation.Classification.Classifier.OptimizedKPSetClassClassifier") {
						if (!mKPSetClassClassifier) {
							mKPSetClassClassifier = new COptimizedKPSetClassSubsumptionClassifierThread(reasonerMan);
						}
						classifier = mKPSetClassClassifier;
					} else if (classifierName == "Konclude.Calculation.Classification.Classifier.OptimizedClassExtractedSaturationClassifier") {
						if (!mExtractedSaturationClassClassifier) {
							mExtractedSaturationClassClassifier = new COptimizedClassExtractedSaturationSubsumptionClassifierThread(reasonerMan);
						}
						classifier = mExtractedSaturationClassClassifier;
					}
				}
				return classifier;
			}

			CSubsumptionClassifier *CConfigDependedSubsumptionClassifierFactory::createBackgroundClassifier(CConcreteOntology *ontology, CConfigurationBase *config) {
				CSubsumptionClassifier *classifier = createClassifier(ontology,config);
				return classifier;
			}


			bool CConfigDependedSubsumptionClassifierFactory::isClassificationBySatisfiableCalculationSufficient(CConcreteOntology *ontology, CConfigurationBase *config) {
				COntologyStructureSummary* ontoStrSum = ontology->getStructureSummary();
				if (ontoStrSum) {
					return ontoStrSum->hasOnlyDeterministicConceptClasses();
				} else {
					return false;
				}
			}

			bool CConfigDependedSubsumptionClassifierFactory::isClassificationBySaturationCalculationSufficient(CConcreteOntology *ontology, CConfigurationBase *config) {

				bool subsumerSaturationExtraction = CConfigDataReader::readConfigBoolean(config,"Konclude.Calculation.Classification.SaturationSubsumerExtraction",true);
				if (!subsumerSaturationExtraction) {
					return false;
				}


				CSaturationData* satData = ontology->getPrecomputation()->getSaturationModelData();
				if (satData) {
					CSaturationTaskData* satTaskData = dynamic_cast<CSaturationTaskData*>(satData);
					if (satTaskData) {
						CSatisfiableCalculationTask* satCalcTask = satTaskData->getSaturationTask();
						if (!satCalcTask->getProcessingDataBox()->isInsufficientNodeOccured() && !satCalcTask->getProcessingDataBox()->isProblematicEQCandidateOccured()) {
							CBOXSET<CConcept*>* eqConceptNonCandSet = ontology->getTBox()->getEquivalentConceptNonCandidateSet(false);
							if (!eqConceptNonCandSet || eqConceptNonCandSet->isEmpty()) {
								return true;
							}
						}
					}
				}
				return false;
			}




			CSubsumptionClassifier* CConfigDependedSubsumptionClassifierFactory::getDataPropertyClassifier(CConcreteOntology *ontology, CConfigurationBase *config) {
				if (!mDataPropertyClassifier) {
					mDataPropertyClassifier = new COptimizedKPSetRoleSubsumptionClassifierThread(reasonerMan,true);
				}
				return mDataPropertyClassifier;
			}			
			
			CSubsumptionClassifier* CConfigDependedSubsumptionClassifierFactory::getObjectPropertyClassifier(CConcreteOntology *ontology, CConfigurationBase *config) {
				if (!mObjectPropertyClassifier) {
					mObjectPropertyClassifier = new COptimizedKPSetRoleSubsumptionClassifierThread(reasonerMan,false);
				}
				return mObjectPropertyClassifier;
			}



		}; // end namespace Classifier

	}; // end namespace Reasoner

}; // end namespace Konclude
