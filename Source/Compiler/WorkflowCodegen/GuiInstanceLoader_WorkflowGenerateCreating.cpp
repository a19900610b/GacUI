#include "GuiInstanceLoader_WorkflowCodegen.h"
#include "../../Reflection/TypeDescriptors/GuiReflectionEvents.h"
#include "../../Resources/GuiParserManager.h"

namespace vl
{
	namespace presentation
	{
		using namespace collections;
		using namespace parsing;
		using namespace workflow;
		using namespace workflow::analyzer;
		using namespace reflection::description;

/***********************************************************************
WorkflowGenerateCreatingVisitor
***********************************************************************/

		class WorkflowGenerateCreatingVisitor : public Object, public GuiValueRepr::IVisitor
		{
		public:
			GuiResourcePrecompileContext&		precompileContext;
			types::ResolvingResult&				resolvingResult;
			Ptr<WfBlockStatement>				statements;
			GuiResourceError::List&				errors;
			
			WorkflowGenerateCreatingVisitor(GuiResourcePrecompileContext& _precompileContext, types::ResolvingResult& _resolvingResult, Ptr<WfBlockStatement> _statements, GuiResourceError::List& _errors)
				:precompileContext(_precompileContext)
				, resolvingResult(_resolvingResult)
				, errors(_errors)
				, statements(_statements)
			{
			}

			///////////////////////////////////////////////////////////////////////////////////

			IGuiInstanceLoader::ArgumentInfo GetArgumentInfo(GuiResourceTextPos attPosition, GuiValueRepr* repr)
			{
				Ptr<ITypeInfo> typeInfo = nullptr;
				bool serializable = false;
				WString textValue;
				GuiResourceTextPos textValuePosition;
				GuiConstructorRepr* ctor = nullptr;

				if (auto text = dynamic_cast<GuiTextRepr*>(repr))
				{
					typeInfo = resolvingResult.propertyResolvings[repr].info->acceptableTypes[0];
					serializable = true;
					textValue = text->text;
					textValuePosition = text->tagPosition;
				}
				else if ((ctor = dynamic_cast<GuiConstructorRepr*>(repr)))
				{
					if (ctor->instanceName == GlobalStringKey::Empty)
					{
						typeInfo = resolvingResult.propertyResolvings[repr].info->acceptableTypes[0];
					}
					else
					{
						typeInfo = resolvingResult.typeInfos[ctor->instanceName].typeInfo;
					}

					if ((typeInfo->GetTypeDescriptor()->GetTypeDescriptorFlags() & TypeDescriptorFlags::StructType) != TypeDescriptorFlags::Undefined)
					{
						serializable = true;
						auto value = ctor->setters.Values()[0]->values[0].Cast<GuiTextRepr>();
						textValue = value->text;
						textValuePosition = value->tagPosition;
					}
				}

				IGuiInstanceLoader::ArgumentInfo argumentInfo;
				argumentInfo.typeInfo = typeInfo;
				argumentInfo.attPosition = attPosition;

				if (serializable)
				{
					if (auto deserializer = GetInstanceLoaderManager()->GetInstanceDeserializer(typeInfo.Obj()))
					{
						auto typeInfoAs = deserializer->DeserializeAs(typeInfo.Obj());
						if (auto expression = Workflow_ParseTextValue(precompileContext, typeInfoAs->GetTypeDescriptor(), { resolvingResult.resource }, textValue, textValuePosition, errors))
						{
							argumentInfo.expression = deserializer->Deserialize(precompileContext, resolvingResult, typeInfo.Obj(), expression, textValuePosition, errors);
						}
					}
					else
					{
						argumentInfo.expression = Workflow_ParseTextValue(precompileContext, typeInfo->GetTypeDescriptor(), { resolvingResult.resource }, textValue, textValuePosition, errors);
					}
					argumentInfo.valuePosition = textValuePosition;
				}
				else
				{
					repr->Accept(this);

					auto ref = MakePtr<WfReferenceExpression>();
					ref->name.value = ctor->instanceName.ToString();
					argumentInfo.expression = ref;
				}

				if (argumentInfo.expression)
				{
					Workflow_RecordScriptPosition(precompileContext, repr->tagPosition, argumentInfo.expression);
				}
				return argumentInfo;
			}

			///////////////////////////////////////////////////////////////////////////////////

			Ptr<WfStatement> ProcessPropertySet(
				IGuiInstanceLoader::PropertyInfo propInfo,
				GuiAttSetterRepr* repr,
				Ptr<GuiAttSetterRepr::SetterValue> setter,
				GuiAttSetterRepr* setTarget
				)
			{
				auto info = resolvingResult.propertyResolvings[setTarget];
				vint errorCount = errors.Count();
				if (auto expr = info.loader->GetParameter(precompileContext, resolvingResult, propInfo, repr->instanceName, setter->attPosition, errors))
				{
					auto refInstance = MakePtr<WfReferenceExpression>();
					refInstance->name.value = setTarget->instanceName.ToString();

					auto assign = MakePtr<WfBinaryExpression>();
					assign->op = WfBinaryOperator::Assign;
					assign->first = refInstance;
					assign->second = expr;

					auto stat = MakePtr<WfExpressionStatement>();
					stat->expression = assign;

					return stat;
				}
				else if (errorCount == errors.Count())
				{
					errors.Add(GuiResourceError({ resolvingResult.resource }, setTarget->tagPosition,
						L"[INTERNAL ERROR] Precompile: Something is wrong when retriving the property \"" +
						propInfo.propertyName.ToString() +
						L"\" from an instance of type \"" +
						propInfo.typeInfo.typeName.ToString() +
						L"\"."));
				}
				return nullptr;
			}

			///////////////////////////////////////////////////////////////////////////////////

			Ptr<WfStatement> ProcessPropertyCollection(
				IGuiInstanceLoader::PropertyInfo propInfo,
				GuiAttSetterRepr* repr,
				Group<GlobalStringKey, IGuiInstanceLoader*>& usedProps,
				Ptr<GuiAttSetterRepr::SetterValue> setter,
				types::PropertyResolving info,
				Ptr<GuiValueRepr> value
				)
			{
				if (!usedProps.Contains(propInfo.propertyName, info.loader))
				{
					usedProps.Add(propInfo.propertyName, info.loader);
				}

				vint errorCount = errors.Count();
				IGuiInstanceLoader::ArgumentMap arguments;
				arguments.Add(propInfo.propertyName, GetArgumentInfo(setter->attPosition, value.Obj()));
				if (auto stat = info.loader->AssignParameters(precompileContext, resolvingResult, propInfo.typeInfo, repr->instanceName, arguments, setter->attPosition, errors))
				{
					return stat;
				}
				else if (errorCount == errors.Count())
				{
					errors.Add(GuiResourceError({ resolvingResult.resource }, value->tagPosition,
						L"[INTERNAL ERROR] Precompile: Something is wrong when assigning to property " +
						propInfo.propertyName.ToString() +
						L" to an instance of type \"" +
						propInfo.typeInfo.typeName.ToString() +
						L"\"."));
				}
				return nullptr;
			}

			///////////////////////////////////////////////////////////////////////////////////

			Ptr<WfStatement> ProcessPropertyOthers(
				IGuiInstanceLoader::PropertyInfo propInfo,
				GuiAttSetterRepr* repr,
				Group<GlobalStringKey, IGuiInstanceLoader*>& usedProps,
				Ptr<GuiAttSetterRepr::SetterValue> setter,
				types::PropertyResolving info,
				Ptr<GuiValueRepr> value
				)
			{
				List<GlobalStringKey> pairedProps;
				info.loader->GetPairedProperties(propInfo, pairedProps);
				if (pairedProps.Count() == 0)
				{
					pairedProps.Add(propInfo.propertyName);
				}

				vint errorCount = errors.Count();
				IGuiInstanceLoader::ArgumentMap arguments;
				FOREACH(GlobalStringKey, pairedProp, pairedProps)
				{
					usedProps.Add(pairedProp, info.loader);
					auto pairedSetter = repr->setters[pairedProp];
					FOREACH(Ptr<GuiValueRepr>, pairedValue, pairedSetter->values)
					{
						auto pairedInfo = resolvingResult.propertyResolvings[pairedValue.Obj()];
						if (pairedInfo.loader == info.loader)
						{
							arguments.Add(pairedProp, GetArgumentInfo(pairedSetter->attPosition, pairedValue.Obj()));
						}
					}
				}

				if (auto stat = info.loader->AssignParameters(precompileContext, resolvingResult, propInfo.typeInfo, repr->instanceName, arguments, setter->attPosition, errors))
				{
					return stat;
				}
				else if (errorCount == errors.Count())
				{
					WString propNames;
					FOREACH_INDEXER(GlobalStringKey, pairedProp, propIndex, pairedProps)
					{
						if (propIndex > 0)propNames += L", ";
						propNames += L"\"" + pairedProp.ToString() + L"\"";
					}
					errors.Add(GuiResourceError({ resolvingResult.resource }, value->tagPosition,
						L"[INTERNAL ERROR] Precompile: Something is wrong when assigning to properties " +
						propNames +
						L" to an instance of type \"" +
						propInfo.typeInfo.typeName.ToString() +
						L"\"."));
				}
				return nullptr;
			}

			///////////////////////////////////////////////////////////////////////////////////

			void Visit(GuiTextRepr* repr)override
			{
			}

			void Visit(GuiAttSetterRepr* repr)override
			{
				auto reprTypeInfo = resolvingResult.typeInfos[repr->instanceName];
				
				if (reprTypeInfo.typeInfo && (reprTypeInfo.typeInfo->GetTypeDescriptor()->GetTypeDescriptorFlags() & TypeDescriptorFlags::ReferenceType) != TypeDescriptorFlags::Undefined)
				{
					WORKFLOW_ENVIRONMENT_VARIABLE_ADD

					Group<GlobalStringKey, IGuiInstanceLoader*> usedProps;
					FOREACH(GlobalStringKey, prop, From(repr->setters.Keys()).Reverse())
					{
						auto setter = repr->setters[prop];
						IGuiInstanceLoader::PropertyInfo propInfo(reprTypeInfo, prop);
						if (setter->binding == GlobalStringKey::_Set)
						{
							auto setTarget = dynamic_cast<GuiAttSetterRepr*>(setter->values[0].Obj());
							if (auto statement = ProcessPropertySet(propInfo, repr, setter, setTarget))
							{
								Workflow_RecordScriptPosition(precompileContext, setTarget->tagPosition, statement);
								statements->statements.Add(statement);
							}
							setTarget->Accept(this);
						}
						else if (setter->binding == GlobalStringKey::Empty)
						{
							FOREACH(Ptr<GuiValueRepr>, value, setter->values)
							{
								auto info = resolvingResult.propertyResolvings[value.Obj()];
								if (info.info->usage == GuiInstancePropertyInfo::Property)
								{
									if (info.info->support == GuiInstancePropertyInfo::SupportCollection)
									{
										if (auto statement = ProcessPropertyCollection(propInfo, repr, usedProps, setter, info, value))
										{
											Workflow_RecordScriptPosition(precompileContext, value->tagPosition, statement);
											statements->statements.Add(statement);
										}
									}
									else if (!usedProps.Contains(prop, info.loader))
									{
										if (auto statement = ProcessPropertyOthers(propInfo, repr, usedProps, setter, info, value))
										{
											Workflow_RecordScriptPosition(precompileContext, value->tagPosition, statement);
											statements->statements.Add(statement);
										}
									}
								}
							}
						}
					}

					WORKFLOW_ENVIRONMENT_VARIABLE_REMOVE
				}
			}

			void FillCtorArguments(GuiConstructorRepr* repr, IGuiInstanceLoader* loader, const IGuiInstanceLoader::TypeInfo& typeInfo, IGuiInstanceLoader::ArgumentMap& arguments)
			{
				List<GlobalStringKey> ctorProps;
				loader->GetPropertyNames(typeInfo, ctorProps);

				WORKFLOW_ENVIRONMENT_VARIABLE_ADD

				FOREACH(GlobalStringKey, prop, ctorProps)
				{
					auto propInfo = loader->GetPropertyType({ typeInfo,prop });
					if (propInfo->usage != GuiInstancePropertyInfo::ConstructorArgument) continue;

					auto index = repr->setters.Keys().IndexOf(prop);
					if (index == -1) continue;

					auto setter = repr->setters.Values()[index];
					if (setter->binding == GlobalStringKey::Empty)
					{
						FOREACH(Ptr<GuiValueRepr>, value, setter->values)
						{
							auto argument = GetArgumentInfo(setter->attPosition, value.Obj());
							if (argument.typeInfo && argument.expression)
							{
								arguments.Add(prop, argument);
							}
						}
					}
					else if (auto binder = GetInstanceLoaderManager()->GetInstanceBinder(setter->binding))
					{
						auto propInfo = IGuiInstanceLoader::PropertyInfo(typeInfo, prop);
						auto resolvedPropInfo = loader->GetPropertyType(propInfo);
						auto value = setter->values[0].Cast<GuiTextRepr>();
						if (auto expression = binder->GenerateConstructorArgument(precompileContext, resolvingResult, loader, propInfo, resolvedPropInfo, value->text, value->tagPosition, errors))
						{
							Workflow_RecordScriptPosition(precompileContext, value->tagPosition, expression);

							IGuiInstanceLoader::ArgumentInfo argument;
							argument.expression = expression;
							argument.typeInfo = resolvedPropInfo->acceptableTypes[0];
							argument.attPosition = setter->attPosition;
							arguments.Add(prop, argument);
						}
					}
					else
					{
						errors.Add(GuiResourceError({ resolvingResult.resource }, setter->attPosition,
							L"[INTERNAL ERROR] Precompile: The appropriate IGuiInstanceBinder of binding \"-" +
							setter->binding.ToString() +
							L"\" cannot be found."));
					}
				}

				WORKFLOW_ENVIRONMENT_VARIABLE_REMOVE
			}

			void Visit(GuiConstructorRepr* repr)override
			{
				IGuiInstanceLoader::TypeInfo ctorTypeInfo;
				if (resolvingResult.context->instance.Obj() == repr)
				{
					auto source = FindInstanceLoadingSource(resolvingResult.context, repr);
					ctorTypeInfo.typeName = source.typeName;

					auto typeInfo = GetInstanceLoaderManager()->GetTypeInfoForType(source.typeName);
					ctorTypeInfo.typeInfo = typeInfo;
				}
				else
				{
					ctorTypeInfo = resolvingResult.typeInfos[repr->instanceName];
				}

				auto ctorLoader = GetInstanceLoaderManager()->GetLoader(ctorTypeInfo.typeName);
				while (ctorLoader)
				{
					if (ctorLoader->CanCreate(ctorTypeInfo))
					{
						break;
					}
					ctorLoader = GetInstanceLoaderManager()->GetParentLoader(ctorLoader);
				}

				if (resolvingResult.context->instance.Obj() == repr)
				{
					resolvingResult.rootLoader = ctorLoader;
					FillCtorArguments(repr, ctorLoader, ctorTypeInfo, resolvingResult.rootCtorArguments);

					{
						auto refInstance = MakePtr<WfReferenceExpression>();
						refInstance->name.value = repr->instanceName.ToString();

						auto refThis = MakePtr<WfReferenceExpression>();
						refThis->name.value = L"<this>";

						auto assign = MakePtr<WfBinaryExpression>();
						assign->op = WfBinaryOperator::Assign;
						assign->first = refInstance;
						assign->second = refThis;

						auto stat = MakePtr<WfExpressionStatement>();
						stat->expression = assign;

						statements->statements.Add(stat);
					}

					if (resolvingResult.rootCtorArguments.Count() > 0)
					{
						if (auto stat = ctorLoader->InitializeRootInstance(precompileContext, resolvingResult, ctorTypeInfo, repr->instanceName, resolvingResult.rootCtorArguments, errors))
						{
							Workflow_RecordScriptPosition(precompileContext, resolvingResult.context->tagPosition, stat);
							statements->statements.Add(stat);
						}
					}

					FOREACH(Ptr<GuiInstanceParameter>, parameter, resolvingResult.context->parameters)
					{
						auto refInstance = MakePtr<WfReferenceExpression>();
						refInstance->name.value = parameter->name.ToString();

						auto refThis = MakePtr<WfReferenceExpression>();
						refThis->name.value = L"<this>";

						auto refParameter = MakePtr<WfMemberExpression>();
						refParameter->parent = refThis;
						refParameter->name.value = parameter->name.ToString();

						auto assign = MakePtr<WfBinaryExpression>();
						assign->op = WfBinaryOperator::Assign;
						assign->first = refInstance;
						assign->second = refParameter;

						auto stat = MakePtr<WfExpressionStatement>();
						stat->expression = assign;

						statements->statements.Add(stat);
						Workflow_RecordScriptPosition(precompileContext, parameter->tagPosition, (Ptr<WfStatement>)stat);
					}
				}
				else
				{
					IGuiInstanceLoader::ArgumentMap arguments;
					FillCtorArguments(repr, ctorLoader, ctorTypeInfo, arguments);

					vint errorCount = errors.Count();
					if (auto ctorStats = ctorLoader->CreateInstance(precompileContext, resolvingResult, ctorTypeInfo, repr->instanceName, arguments, repr->tagPosition, errors))
					{
						Workflow_RecordScriptPosition(precompileContext, resolvingResult.context->tagPosition, ctorStats);
						statements->statements.Add(ctorStats);
					}
					else if (errorCount == errors.Count())
					{
						errors.Add(GuiResourceError({ resolvingResult.resource }, repr->tagPosition,
							L"[INTERNAL ERROR] Precompile: Something is wrong when creating an instance of type \"" +
							ctorTypeInfo.typeName.ToString() +
							L"\"."));
					}
				}
				Visit((GuiAttSetterRepr*)repr);
			}
		};

		void Workflow_GenerateCreating(GuiResourcePrecompileContext& precompileContext, types::ResolvingResult& resolvingResult, Ptr<WfBlockStatement> statements, GuiResourceError::List& errors)
		{
			WorkflowGenerateCreatingVisitor visitor(precompileContext, resolvingResult, statements, errors);
			resolvingResult.context->instance->Accept(&visitor);
		}
	}
}
